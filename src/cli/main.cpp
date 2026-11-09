// SPDX-License-Identifier: GPL-2.0-or-later
//
// x52ctl - command line control for the X52 Pro daemon.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "x52/client.hpp"
#include "x52/profile.hpp"
#include "x52/protocol.hpp"

using json = nlohmann::json;
using namespace x52;

namespace {

void usage() {
    std::cout <<
        "x52ctl - control the Logitech/Saitek X52 Pro\n"
        "\n"
        "Usage: x52ctl <command> [arguments]\n"
        "\n"
        "Display\n"
        "  status                       show device, mode and profile state\n"
        "  text <l1> [l2] [l3]          write the three MFD lines\n"
        "  page <clock|stopwatch|status|custom|profile>\n"
        "  bright <mfd> [led]           brightness 0-255\n"
        "  clock [--offset2 <min>] [--offset3 <min>] [--12h]\n"
        "  stopwatch <start|stop|reset>\n"
        "\n"
        "Lights\n"
        "  led <fire|a|b|d|e|t1|t2|t3|pov|clutch|throttle|all> <off|red|amber|green>\n"
        "  shift <on|off>\n"
        "\n"
        "Modes and profiles\n"
        "  mode <1|2|3>\n"
        "  profile list\n"
        "  profile load <name>\n"
        "  profile save [name]\n"
        "  profile show                 dump the active profile as JSON\n"
        "  profile new <name>\n"
        "  profile delete <name>\n"
        "  profile export <file.json>\n"
        "  profile import <file.json>\n"
        "\n"
        "Diagnostics\n"
        "  watch                        stream state changes from the daemon\n"
        "  raw <cmd-hex> <value-hex>    send a raw vendor command\n"
        "  reconnect                    ask the daemon to re-open the device\n"
        "  json <request-json>          send a raw request\n";
}

int fail(const std::string& message) {
    std::cerr << "x52ctl: " << message << "\n";
    return 1;
}

std::string pad(std::string s, std::size_t n) {
    if (s.size() < n) s.append(n - s.size(), ' ');
    return s;
}

json call_or_die(Client& c, const std::string& cmd, const json& args, bool& ok, std::string& error) {
    json result;
    ok = c.request(cmd, args, result, error);
    return result;
}

void print_status(const json& st) {
    std::cout << "Device      : " << (st.value("connected", false) ? "connected" : "DISCONNECTED");
    if (st.contains("last_error") && !st["last_error"].get<std::string>().empty()) {
        std::cout << " (" << st["last_error"].get<std::string>() << ")";
    }
    std::cout << "\n";
    std::cout << "Profile     : " << st.value("profile", std::string("-"));
    if (st.value("profile_dirty", false)) std::cout << " (modified)";
    std::cout << "\n";
    std::cout << "Mode        : " << st.value("mode", 1)
              << "   shift: " << (st.value("shift", false) ? "on" : "off")
              << "   page: " << st.value("page", std::string("-")) << "\n";
    std::cout << "Grabbed     : " << (st.value("grabbed", false) ? "yes" : "no") << "\n";

    std::cout << "\nMFD  +------------------+\n";
    if (st.contains("mfd")) {
        for (const auto& line : st["mfd"]) {
            std::string l = line.get<std::string>();
            if (l.size() < 16) l.append(16 - l.size(), ' ');
            std::cout << "     |" << l << "|\n";
        }
    }
    std::cout << "     +------------------+\n";

    if (st.contains("axes")) {
        std::cout << "\nAxes\n";
        for (const auto& [name, value] : st["axes"].items()) {
            double v = value.get<double>();
            int filled = static_cast<int>(v * 30.0 + 0.5);
            std::string bar(static_cast<std::size_t>(filled), '#');
            bar.append(static_cast<std::size_t>(30 - filled), '.');
            char pct[8];
            std::snprintf(pct, sizeof(pct), "%3d%%", static_cast<int>(v * 100));
            std::cout << "  " << pad(name, 10) << " [" << bar << "] " << pct << "\n";
        }
    }

    if (st.contains("leds")) {
        std::cout << "\nLEDs\n  ";
        bool first = true;
        for (const auto& [name, color] : st["leds"].items()) {
            std::string c = color.get<std::string>();
            if (c == "off") continue;
            if (!first) std::cout << "  ";
            std::cout << name << "=" << c;
            first = false;
        }
        if (first) std::cout << "(all off)";
        std::cout << "\n";
    }

    if (st.contains("buttons") && !st["buttons"].empty()) {
        std::cout << "\nPressed: ";
        for (const auto& b : st["buttons"]) {
            int idx = b.get<int>();
            if (idx >= 0 && idx < static_cast<int>(kButtonCount)) {
                std::cout << button_name(static_cast<Button>(idx)) << " ";
            }
        }
        std::cout << "\n";
    }
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << content;
    return static_cast<bool>(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];

    Client c;
    std::string error;
    if (!c.connect(default_socket_path())) {
        std::cerr << "x52ctl: " << c.error() << "\n"
                  << "Is x52d running?  Try: systemctl --user start x52d\n";
        return 1;
    }

    json result;
    bool ok = false;

    if (cmd == "status" || cmd == "state") {
        ok = c.request("get_state", result, error);
        if (!ok) return fail(error);
        print_status(result);
        return 0;
    }

    if (cmd == "text") {
        json lines = json::array();
        for (int i = 2; i < argc; ++i) lines.push_back(argv[i]);
        ok = c.request("set_text", {{"lines", lines}}, result, error);
        return ok ? 0 : fail(error);
    }

    if (cmd == "page") {
        if (argc < 3) return fail("page needs a name");
        ok = c.request("set_page", {{"page", argv[2]}}, result, error);
        if (!ok) return fail(error);
        std::cout << "page: " << result.value("page", std::string("?")) << "\n";
        return 0;
    }

    if (cmd == "bright") {
        if (argc < 3) return fail("bright needs a value 0-255");
        json args{{"mfd", std::atoi(argv[2])}};
        if (argc > 3) args["led"] = std::atoi(argv[3]);
        ok = c.request("set_brightness", args, result, error);
        return ok ? 0 : fail(error);
    }

    if (cmd == "led") {
        if (argc < 4) return fail("led needs <name|all> <colour>");
        ok = c.request("set_led", {{"led", argv[2]}, {"color", argv[3]}}, result, error);
        return ok ? 0 : fail(error);
    }

    if (cmd == "shift") {
        bool on = argc > 2 && std::string(argv[2]) == "on";
        ok = c.request("set_shift", {{"on", on}}, result, error);
        return ok ? 0 : fail(error);
    }

    if (cmd == "mode") {
        if (argc < 3) return fail("mode needs 1, 2 or 3");
        ok = c.request("set_mode", {{"mode", std::atoi(argv[2])}}, result, error);
        if (!ok) return fail(error);
        std::cout << "mode: " << result.value("mode", 1) << "\n";
        return 0;
    }

    if (cmd == "clock") {
        json args = json::object();
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--offset2" && i + 1 < argc) args["clock2_offset"] = std::atoi(argv[++i]);
            else if (a == "--offset3" && i + 1 < argc) args["clock3_offset"] = std::atoi(argv[++i]);
            else if (a == "--12h") args["h24"] = false;
            else if (a == "--24h") args["h24"] = true;
        }
        ok = c.request("set_clock", args, result, error);
        return ok ? 0 : fail(error);
    }

    if (cmd == "stopwatch") {
        std::string op = argc > 2 ? argv[2] : "start";
        ok = c.request("stopwatch", {{"op", op}}, result, error);
        if (!ok) return fail(error);
        std::cout << "stopwatch " << op << " ("
                  << (result.value("running", false) ? "running" : "stopped") << ", "
                  << result.value("elapsed_ms", 0) << " ms)\n";
        return 0;
    }

    if (cmd == "raw") {
        if (argc < 4) return fail("raw needs <cmd-hex> <value-hex>");
        auto cmdv = static_cast<int>(std::strtol(argv[2], nullptr, 16));
        auto valv = static_cast<int>(std::strtol(argv[3], nullptr, 16));
        ok = c.request("raw", {{"command", cmdv}, {"value", valv}}, result, error);
        if (!ok) return fail(error);
        std::cout << "raw -> " << result.value("error", std::string("?")) << " "
                  << result.value("message", std::string()) << "\n";
        return 0;
    }

    if (cmd == "reconnect") {
        ok = c.request("reconnect", result, error);
        return ok ? 0 : fail(error);
    }

    if (cmd == "json") {
        if (argc < 3) return fail("json needs a request object");
        json req;
        try {
            req = json::parse(argv[2]);
        } catch (const std::exception& e) {
            return fail(std::string("bad json: ") + e.what());
        }
        std::string c2 = req.value("cmd", std::string());
        json args = req.value("args", json::object());
        ok = c.request(c2, args, result, error);
        if (!ok) return fail(error);
        std::cout << result.dump(2) << "\n";
        return 0;
    }

    if (cmd == "profile") {
        std::string action = argc > 2 ? argv[2] : "list";

        if (action == "list") {
            ok = c.request("profile", {{"action", "list"}}, result, error);
            if (!ok) return fail(error);
            std::cout << "Profiles in " << profile_directory() << "\n";
            for (const auto& p : result["profiles"]) {
                std::cout << (p.get<std::string>() == result.value("current", std::string())
                                  ? "  * " : "    ")
                          << p.get<std::string>() << "\n";
            }
            return 0;
        }
        if (action == "load") {
            if (argc < 4) return fail("profile load needs a name");
            ok = c.request("profile", {{"action", "load"}, {"name", argv[3]}}, result, error);
            if (!ok) return fail(error);
            std::cout << "loaded: " << result.value("profile", std::string()) << "\n";
            return 0;
        }
        if (action == "save") {
            json args{{"action", "save"}};
            if (argc > 3) args["name"] = argv[3];
            ok = c.request("profile", args, result, error);
            if (!ok) return fail(error);
            std::cout << "saved: " << result.value("profile", std::string()) << "\n";
            return 0;
        }
        if (action == "show") {
            ok = c.request("profile", {{"action", "get"}}, result, error);
            if (!ok) return fail(error);
            std::cout << result["profile"].dump(2) << "\n";
            return 0;
        }
        if (action == "new") {
            if (argc < 4) return fail("profile new needs a name");
            ok = c.request("profile", {{"action", "create"}, {"name", argv[3]}}, result, error);
            if (!ok) return fail(error);
            std::cout << "created: " << result.value("profile", std::string()) << "\n";
            return 0;
        }
        if (action == "delete") {
            if (argc < 4) return fail("profile delete needs a name");
            ok = c.request("profile", {{"action", "delete"}, {"name", argv[3]}}, result, error);
            if (!ok) return fail(error);
            std::cout << "deleted\n";
            return 0;
        }
        if (action == "export") {
            if (argc < 4) return fail("profile export needs a file path");
            ok = c.request("profile", {{"action", "get"}}, result, error);
            if (!ok) return fail(error);
            if (!write_file(argv[3], result["profile"].dump(2) + "\n")) {
                return fail(std::string("cannot write ") + argv[3]);
            }
            std::cout << "exported " << result.value("name", std::string()) << " -> " << argv[3]
                      << "\n";
            return 0;
        }
        if (action == "import") {
            if (argc < 4) return fail("profile import needs a file path");
            std::string text;
            if (!read_file(argv[3], text)) return fail(std::string("cannot read ") + argv[3]);
            json prof;
            try {
                prof = json::parse(text);
            } catch (const std::exception& e) {
                return fail(std::string("invalid JSON: ") + e.what());
            }
            ok = c.request("profile", {{"action", "set"}, {"profile", prof}}, result, error);
            if (!ok) return fail(error);
            ok = c.request("profile", {{"action", "save"}, {"name", prof.value("name", "Imported")}},
                           result, error);
            if (!ok) return fail(error);
            std::cout << "imported " << result.value("profile", std::string()) << "\n";
            return 0;
        }
        return fail("unknown profile action '" + action + "'");
    }

    if (cmd == "watch") {
        Client sub;
        if (!sub.connect(default_socket_path(), 0)) return fail(sub.error());
        json r;
        if (!sub.request("subscribe", r, error)) return fail(error);
        std::cout << "Streaming state (Ctrl-C to stop)\n";
        std::cout.flush();
        std::string line;
        while (sub.next_line(line, error)) {
            json ev;
            try {
                ev = json::parse(line);
            } catch (...) {
                continue;
            }
            if (!ev.contains("event") || ev["event"] != "state") continue;
            const json& d = ev["data"];
            std::string mfd0 = d["mfd"][0].get<std::string>();
            std::cout << "mode=" << d.value("mode", 1) << " shift="
                      << (d.value("shift", false) ? "on " : "off")
                      << " page=" << pad(d.value("page", std::string("-")), 9)
                      << " thr=" << static_cast<int>(d["axes"]["throttle"].get<double>() * 100)
                      << "%  [" << pad(mfd0, 16) << "]\r";
            std::cout.flush();
        }
        return fail(error);
    }

    usage();
    return 2;
}
