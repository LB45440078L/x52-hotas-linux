// SPDX-License-Identifier: BSD-3-Clause
#include "x52/telemetry.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <nlohmann/json.hpp>

namespace x52 {
namespace {

using json = nlohmann::json;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// War Thunder publishes some keys with spaces, commas and percent signs.
bool lookup(const json& j, const char* key, double& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    out = it->get<double>();
    return true;
}

}  // namespace

const char* telemetry_source_name(TelemetrySourceKind kind) noexcept {
    switch (kind) {
        case TelemetrySourceKind::None: return "none";
        case TelemetrySourceKind::WarThunder: return "warthunder";
    }
    return "none";
}

bool telemetry_source_from_name(std::string_view name, TelemetrySourceKind& out) noexcept {
    std::string s = lower(std::string(name));
    if (s == "none" || s.empty()) {
        out = TelemetrySourceKind::None;
        return true;
    }
    if (s == "warthunder" || s == "war_thunder" || s == "wt") {
        out = TelemetrySourceKind::WarThunder;
        return true;
    }
    return false;
}

bool http_get(const std::string& host, int port, const std::string& path, std::string& body,
              int timeout_ms) {
    body.clear();

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) return false;

    int fd = ::socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return false;
    }

    // Non-blocking connect with a deadline, so a missing game costs us a few
    // hundred microseconds instead of a stalled poll.
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);

    if (rc != 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return false;
        }
        struct pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) {
            ::close(fd);
            return false;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            ::close(fd);
            return false;
        }
    }
    ::fcntl(fd, F_SETFL, flags);

    std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nConnection: close\r\nUser-Agent: x52pro-linux\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        ::close(fd);
        return false;
    }

    std::string raw;
    char buf[8192];
    for (;;) {
        struct pollfd pfd{fd, POLLIN, 0};
        int ready = ::poll(&pfd, 1, timeout_ms);
        if (ready <= 0) break;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            raw.append(buf, static_cast<std::size_t>(n));
            if (raw.size() > (1u << 20)) break;
            continue;
        }
        break;
    }
    ::close(fd);

    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;
    std::string headers = raw.substr(0, header_end);
    if (headers.find("200") == std::string::npos) return false;

    // Honour Content-Length when present; the connection is closed either way.
    std::size_t length = std::string::npos;
    std::istringstream hs(headers);
    std::string line;
    while (std::getline(hs, line)) {
        std::string l = lower(line);
        if (l.rfind("content-length:", 0) == 0) {
            length = static_cast<std::size_t>(std::strtoul(line.c_str() + 15, nullptr, 10));
        }
    }

    body = raw.substr(header_end + 4);
    if (length != std::string::npos && length <= body.size()) body.resize(length);
    return !body.empty();
}

bool parse_warthunder(const std::string& state_json, const std::string& indicators_json,
                      TelemetryValues& out, std::string& error) {
    out.clear();
    json state;
    try {
        state = json::parse(state_json);
    } catch (const std::exception& e) {
        error = std::string("state: ") + e.what();
        return false;
    }
    if (!state.value("valid", false)) {
        error = "no aircraft in flight";
        return false;
    }

    double v = 0.0;
    auto put = [&](const char* name, const char* key) {
        if (lookup(state, key, v)) out[name] = v;
    };

    put("ias", "IAS, km/h");
    put("tas", "TAS, km/h");
    put("alt", "H, m");
    put("mach", "M");
    put("g", "Ny");
    put("aoa", "AoA, deg");
    put("vy", "Vy, m/s");
    put("fuel", "Mfuel, kg");
    put("flaps", "flaps, %");
    put("aileron", "aileron, %");
    put("elevator", "elevator, %");
    put("rudder", "rudder, %");
    put("throttle", "throttle 1, %");
    put("rpm", "RPM 1");

    double fuel_max = 0.0;
    if (lookup(state, "Mfuel0, kg", fuel_max) && fuel_max > 1.0) {
        double fuel = out.count("fuel") ? out["fuel"] : 0.0;
        out["fuel_pct"] = fuel * 100.0 / fuel_max;
    }
    out["valid"] = 1.0;

    // /indicators carries the gear animation and the horizon; it is optional.
    if (!indicators_json.empty()) {
        try {
            json ind = json::parse(indicators_json);
            double gear = 0.0;
            if (lookup(ind, "gears", gear)) out["gear"] = gear;
            if (lookup(ind, "compass", v)) out["compass"] = v;
            if (lookup(ind, "aviahorizon_roll", v)) out["bank"] = v;
            if (lookup(ind, "airbrake_indicator", v)) out["airbrake"] = v;
            if (lookup(ind, "vario", v)) out["vario"] = v;
            if (lookup(ind, "weapon2", v)) out["weapon2"] = v;
            if (lookup(ind, "weapon3", v)) out["weapon3"] = v;
            if (lookup(ind, "g_meter", v)) out["g_peak"] = v;
        } catch (const std::exception&) {
            // The instrument feed is optional; keep the state values.
        }
    }
    return true;
}

std::string format_telemetry_line(const std::string& tmpl, const TelemetryValues& values) {
    std::string out;
    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] != '{') {
            out.push_back(tmpl[i++]);
            continue;
        }
        std::size_t end = tmpl.find('}', i);
        if (end == std::string::npos) {
            out.append(tmpl.substr(i));
            break;
        }
        std::string token = tmpl.substr(i + 1, end - i - 1);
        i = end + 1;

        std::string name = token;
        std::string spec;
        auto colon = token.find(':');
        if (colon != std::string::npos) {
            name = token.substr(0, colon);
            spec = token.substr(colon + 1);
        }
        name = lower(name);

        auto it = values.find(name);
        if (it == values.end()) {
            out += "--";
            continue;
        }

        char buf[64];
        if (spec.empty()) {
            double v = it->second;
            if (v == static_cast<long long>(v)) std::snprintf(buf, sizeof(buf), "%lld",
                                                              static_cast<long long>(v));
            else std::snprintf(buf, sizeof(buf), "%.1f", v);
        } else if (spec.back() == 'd') {
            std::string fmt = "%" + spec;
            std::snprintf(buf, sizeof(buf), fmt.c_str(), static_cast<long long>(it->second));
        } else {
            std::string fmt = "%" + spec;
            std::snprintf(buf, sizeof(buf), fmt.c_str(), it->second);
        }
        out += buf;
    }
    return out;
}

namespace {

class WarThunderSource : public TelemetrySource {
public:
    TelemetrySourceKind kind() const override { return TelemetrySourceKind::WarThunder; }

    bool poll(TelemetryValues& out, std::string& error) override {
        std::string state_body;
        if (!http_get("127.0.0.1", 8111, "/state", state_body, 400)) {
            error = "localhost:8111 not answering (is a battle running?)";
            return false;
        }
        std::string indicators_body;
        http_get("127.0.0.1", 8111, "/indicators", indicators_body, 300);
        return parse_warthunder(state_body, indicators_body, out, error);
    }
};

}  // namespace

std::unique_ptr<TelemetrySource> make_telemetry_source(TelemetrySourceKind kind) {
    switch (kind) {
        case TelemetrySourceKind::WarThunder: return std::make_unique<WarThunderSource>();
        case TelemetrySourceKind::None: break;
    }
    return nullptr;
}

}  // namespace x52
