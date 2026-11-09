// SPDX-License-Identifier: BSD-3-Clause
#include "x52/client.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace x52 {

std::string default_socket_path() {
    const char* override_path = std::getenv("X52D_SOCKET");
    if (override_path && *override_path) return override_path;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = (xdg && *xdg) ? xdg : "/tmp";
    return dir + "/x52d.sock";
}

bool daemon_is_running() {
    Client c;
    if (!c.connect(default_socket_path(), 300)) return false;
    nlohmann::json result;
    std::string error;
    return c.request("get_state", result, error);
}

Client::~Client() { close(); }

void Client::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    buffer_.clear();
}

bool Client::connect(const std::string& path, int timeout_ms) {
    close();
    error_.clear();

    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        error_ = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        error_ = "socket path too long";
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_ = "cannot connect to " + path + ": " + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
}

bool Client::read_line(std::string& line) {
    for (;;) {
        auto pos = buffer_.find('\n');
        if (pos != std::string::npos) {
            line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            return true;
        }
        char buf[8192];
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            buffer_.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            error_ = "timeout waiting for the daemon";
        } else if (n == 0) {
            error_ = "daemon closed the connection";
        } else {
            error_ = std::string("recv: ") + std::strerror(errno);
        }
        return false;
    }
}

bool Client::next_line(std::string& line, std::string& error) {
    if (!read_line(line)) {
        error = error_;
        return false;
    }
    return true;
}

bool Client::request(const std::string& cmd, const nlohmann::json& args,
                     nlohmann::json& result, std::string& error) {
    if (fd_ < 0) {
        error = error_.empty() ? "not connected" : error_;
        return false;
    }

    int id = next_id_++;
    nlohmann::json req{{"id", id}, {"cmd", cmd}};
    if (!args.empty()) req["args"] = args;
    std::string line = req.dump() + "\n";

    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(line.size())) {
        ssize_t n = ::send(fd_, line.data() + sent, line.size() - static_cast<std::size_t>(sent),
                           MSG_NOSIGNAL);
        if (n > 0) {
            sent += n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        error = std::string("send: ") + std::strerror(errno);
        close();
        return false;
    }

    // Skip asynchronous events until our reply arrives.
    for (int guard = 0; guard < 256; ++guard) {
        std::string reply;
        if (!read_line(reply)) {
            error = error_;
            close();
            return false;
        }
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(reply);
        } catch (const std::exception&) {
            continue;
        }
        if (!j.contains("id") || j["id"] != id) continue;
        if (j.value("ok", false)) {
            result = j.contains("result") ? j["result"] : nlohmann::json::object();
            return true;
        }
        error = j.value("error", std::string("request failed"));
        return false;
    }
    error = "too many unsolicited events";
    return false;
}

}  // namespace x52
