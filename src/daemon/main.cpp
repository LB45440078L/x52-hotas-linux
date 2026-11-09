// SPDX-License-Identifier: BSD-3-Clause
//
// x52d - X52 Pro daemon.
//
// Owns the hardware, runs the mode/profile engine, drives the MFD and LEDs,
// captures the stick into virtual input devices and exposes a small JSON
// control socket for the CLI and the GUI.
//
// Socket protocol: newline delimited JSON, one request per line.
//   {"id": 1, "cmd": "get_state"}
//   {"id": 1, "ok": true, "result": {...}}
//   {"event": "input", "data": {...}}      (unsolicited push)
//
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <mutex>
#include <thread>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <map>

#include <nlohmann/json.hpp>

#include "x52/clock.hpp"
#include "x52/device.hpp"
#include "x52/diagnostics.hpp"
#include "x52/engine.hpp"
#include "x52/input.hpp"
#include "x52/leds.hpp"
#include "x52/mfd.hpp"
#include "x52/profile.hpp"
#include "x52/settings.hpp"
#include "x52/rules.hpp"
#include "x52/telemetry.hpp"
#include "x52/uinput.hpp"

using json = nlohmann::json;
using namespace x52;

namespace {

constexpr int kTickMs = 8;
constexpr uint64_t kMfdRefreshUs = 500000;    // 2 Hz
constexpr uint64_t kStatePushUs = 200000;     // 5 Hz full state
constexpr uint64_t kInputPushUs = 4000;       // 250 Hz cap, device is the limit
constexpr uint64_t kReconnectUs = 2000000;    // retry every 2 s

uint64_t now_us() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

std::string runtime_dir() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) return xdg;
    return "/tmp";
}

std::string socket_path() {
    const char* override_path = std::getenv("X52D_SOCKET");
    if (override_path && *override_path) return override_path;
    return runtime_dir() + "/x52d.sock";
}

// Bridges the engine to real uinput devices.
class UinputSink : public OutputSink {
public:
    explicit UinputSink(Uinput& u) : u_(u) {}

    bool trace = std::getenv("X52D_DEBUG_EVENTS") != nullptr;

    void key(uint16_t code, bool pressed) override {
        if (trace) std::fprintf(stderr, "event: key %u %s\n", code, pressed ? "down" : "up");
        u_.key(code, pressed);
    }
    void mouse_button(uint16_t button, bool pressed) override {
        if (trace) std::fprintf(stderr, "event: mouse %u %s\n", button, pressed ? "down" : "up");
        u_.mouse_button(button, pressed);
    }
    void wheel(int delta) override { u_.wheel(delta); }
    void axis(int index, int value) override { u_.axis(index, value); }
    void button(int index, bool pressed) override {
        if (trace) std::fprintf(stderr, "event: button %d %s\n", index, pressed ? "down" : "up");
        u_.button(index, pressed);
    }
    void hat(int direction) override { u_.hat(direction); }
    void thumb(int x, int y) override {
        if (trace) std::fprintf(stderr, "event: thumb %d,%d\n", x, y);
        u_.hat_secondary(x, y);
    }
    void sync() override { u_.sync_keyboard(); u_.sync_mouse(); }
    void exec(const std::string& command) override {
        pid_t pid = ::fork();
        if (pid == 0) {
            ::setsid();
            ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
    }

private:
    Uinput& u_;
};

struct Client {
    int fd = -1;
    std::string in;
    std::string out;
    bool subscribed = false;
    bool stream = false;   // wants high-rate input frames
};

class Daemon {
public:
    Daemon() : sink_(uinput_), engine_(&sink_), profile_(engine_.profile_mut()) {}

    int run();

private:
    // device lifecycle
    bool try_connect();
    void disconnect(const std::string& reason);
    void flush_outputs();

    // io
    void accept_clients();
    void handle_client(Client& c);
    void handle_request(Client& c, const json& req);
    void reply(Client& c, const json& req, const json& result);
    void reply_error(Client& c, const json& req, const std::string& message);
    void broadcast(const json& event, bool stream_only = false);
    void flush_client(Client& c);

    // state
    json state_json() const;
    void apply_profile(const Profile& p, bool save_as_current);
    void apply_mode(bool force);
    void apply_leds();
    void sync_clock();

    // commands
    json cmd_set_led(const json& args);
    json cmd_set_brightness(const json& args);
    json cmd_profile(const json& args);
    json cmd_text(const json& args);
    json cmd_apply_axis(const json& args);
    json cmd_settings(const json& args);
    json cmd_set_telemetry(const json& args);
    json telemetry_json();
    TelemetrySample telemetry_snapshot() const;
    std::array<std::string, kMfdLineCount> render_lines(uint64_t now) const;
    void apply_rules(uint64_t now);
    json cmd_link_test();
    void mark_dirty();
    json diagnostics_json() const;
    json input_frame() const;

    Device dev_;
    Mfd mfd_;
    Leds leds_;
    Clock clock_;
    InputReader input_;
    Uinput uinput_;
    UinputSink sink_;
    Engine engine_;

    Profile& profile_;
    std::string profile_name_ = "Default";
    bool dirty_profile_ = false;
    bool connected_ = false;
    bool grabbed_ = false;
    std::string last_error_;

    InputState state_{};
    InputState prev_{};
    DriftMonitor drift_;
    std::array<uint64_t, kButtonCount> button_since_{};
    std::vector<std::string> cached_profiles_;
    uint64_t next_profiles_us_ = 0;
    uint64_t last_input_push_us_ = 0;
    AppSettings settings_;
    bool dirty_settings_ = false;
    uint64_t last_change_us_ = 0;
    std::thread tele_thread_;
    mutable std::mutex tele_mutex_;
    TelemetrySample telemetry_;
    TelemetrySourceKind tele_kind_ = TelemetrySourceKind::None;
    bool tele_stop_ = false;
    std::vector<TelemetryRule> rules_;
    std::map<Led, LedColor> forced_leds_;
    uint64_t next_rules_us_ = 0;

    std::array<uint64_t, 512> frame_ring_{};
    std::size_t frame_pos_ = 0;
    std::size_t frame_filled_ = 0;

    std::vector<Client> clients_;
    int listen_fd_ = -1;
    std::string sock_path_;

    uint64_t next_mfd_us_ = 0;
    uint64_t next_clock_us_ = 0;
    uint64_t next_push_us_ = 0;
    uint64_t next_reconnect_us_ = 0;
};

bool Daemon::try_connect() {
    if (!dev_.open()) {
        last_error_ = dev_.error_string();
        return false;
    }
    dev_.set_write_delay_us(2000);

    // Input is a separate node; without it the LEDs still work but there is
    // no live state, no drift measurement and no remapping.
    std::string input_error;
    if (!input_.open(input_error)) {
        std::fprintf(stderr, "x52d: input: %s\n", input_error.c_str());
    } else {
        std::fprintf(stderr, "x52d: input: %s (%s)\n", input_.path().c_str(),
                     input_.is_hidraw() ? "hidraw" : "evdev");
    }

    // Reset everything the device might still hold from another driver.
    mfd_.mark_all_dirty();
    leds_.mark_all_dirty();
    clock_.mark_dirty();
    sync_clock();

    connected_ = true;
    last_error_.clear();

    // Reinstall the LED theme and text for the current mode.
    apply_mode(true);
    apply_leds();
    next_mfd_us_ = 0;

    if (profile_.settings.grab_device) {
        grabbed_ = input_.set_grab(true);
    }

    if (settings_.verify_link) {
        json link = cmd_link_test();
        if (!link.value("valid", true)) {
            std::fprintf(stderr, "x52d: link check: %s (%.2f ms average)\n",
                         link.value("note", std::string("degraded")).c_str(),
                         link.value("avg_ms", 0.0));
        }
    }
    return true;
}

void Daemon::disconnect(const std::string& reason) {
    if (!connected_) return;
    connected_ = false;
    last_error_ = reason;
    dev_.close();
    input_.close();
    grabbed_ = false;
}

void Daemon::flush_outputs() {
    if (!connected_) return;
    DeviceError e = mfd_.flush(dev_);
    if (e == DeviceError::None) e = leds_.flush(dev_);
    if (e == DeviceError::None) e = clock_.flush(dev_);
    if (e != DeviceError::None) {
        disconnect(dev_.error_string());
    }
}

void Daemon::sync_clock() {
    clock_.set_from_system(true);
    clock_.set_offset(1, profile_.settings.clock2_offset,
                      profile_.settings.clock_24h ? ClockFormat::H24 : ClockFormat::H12);
    clock_.set_offset(2, profile_.settings.clock3_offset,
                      profile_.settings.clock_24h ? ClockFormat::H24 : ClockFormat::H12);
}

TelemetrySample Daemon::telemetry_snapshot() const {
    std::lock_guard<std::mutex> lock(tele_mutex_);
    return telemetry_;
}

std::array<std::string, kMfdLineCount> Daemon::render_lines(uint64_t now) const {
    auto lines = engine_.render_mfd(now);
    if (engine_.page() != MfdPage::Telemetry) return lines;

    TelemetrySample snap = telemetry_snapshot();
    if (snap.valid) {
        for (std::size_t i = 0; i < kMfdLineCount; ++i) {
            lines[i] = format_telemetry_line(profile_.settings.telemetry_lines[i], snap.values);
        }
        return lines;
    }
    lines[0] = "NO TELEMETRY";
    lines[1] = snap.connected ? "NO FLIGHT DATA" : "GAME NOT RUNNING";
    lines[2] = "PORT 8111";
    return lines;
}

void Daemon::apply_rules(uint64_t now) {
    TelemetrySample snap = telemetry_snapshot();
    std::map<Led, LedColor> wanted;
    if (snap.valid) wanted = evaluate_rules(rules_, snap.values);

    std::array<LedColor, static_cast<std::size_t>(Led::Count)> theme{};
    bool has_theme = engine_.current_leds(theme);
    auto base = [&](Led led) {
        return has_theme ? theme[static_cast<std::size_t>(led)] : LedColor::Off;
    };

    bool changed = false;
    for (const auto& [led, color] : forced_leds_) {
        if (!wanted.count(led) && leds_.get(led) != base(led)) {
            leds_.set(led, base(led));
            changed = true;
        }
    }
    for (const auto& [led, color] : wanted) {
        if (leds_.get(led) != color) {
            leds_.set(led, color);
            changed = true;
        }
    }
    forced_leds_ = std::move(wanted);

    if (changed && connected_) leds_.flush(dev_);
    next_rules_us_ = now + 200000;
}

void Daemon::mark_dirty() {
    dirty_profile_ = true;
    dirty_settings_ = true;
    last_change_us_ = now_us();
}

void Daemon::apply_mode(bool force) {
    (void)force;
    if (profile_.settings.mode_indicators) apply_leds();
    next_mfd_us_ = 0;  // refresh the display immediately
}

void Daemon::apply_leds() {
    std::array<LedColor, static_cast<std::size_t>(Led::Count)> colors{};
    if (profile_.settings.mode_indicators && engine_.current_leds(colors)) {
        for (std::size_t i = 0; i < colors.size(); ++i) {
            leds_.set(static_cast<Led>(i), colors[i]);
        }
    }
    leds_.set_brightness(static_cast<uint8_t>(profile_.settings.led_brightness));
    leds_.set_shift(engine_.shift_active());
}

void Daemon::apply_profile(const Profile& p, bool save_as_current) {
    if (save_as_current) profile_name_ = p.name;
    engine_.set_profile(p);
    rules_ = p.rules;
    forced_leds_.clear();
    {
        TelemetrySourceKind kind = TelemetrySourceKind::None;
        telemetry_source_from_name(p.settings.telemetry_source, kind);
        std::lock_guard<std::mutex> lock(tele_mutex_);
        if (kind != tele_kind_) {
            tele_kind_ = kind;
            telemetry_ = TelemetrySample{};
            telemetry_.source = telemetry_source_name(kind);
        }
    }
    mfd_.set_brightness(static_cast<uint8_t>(p.settings.mfd_brightness));
    leds_.set_brightness(static_cast<uint8_t>(p.settings.led_brightness));
    sync_clock();
    apply_leds();
    if (!p.settings.grab_device) {
        input_.set_grab(false);
        grabbed_ = false;
    }
    next_mfd_us_ = 0;
}

json Daemon::state_json() const {
    json j;
    j["connected"] = connected_;
    j["last_error"] = last_error_;
    j["mode"] = engine_.mode();
    j["shift"] = engine_.shift_active();
    j["page"] = mfd_page_name(engine_.page());
    j["profile"] = profile_name_;
    j["profile_dirty"] = dirty_profile_;
    j["grabbed"] = grabbed_;

    auto lines = render_lines(now_us());
    j["mfd"] = json::array({lines[0], lines[1], lines[2]});
    j["mfd_brightness"] = profile_.settings.mfd_brightness;
    j["led_brightness"] = profile_.settings.led_brightness;

    json leds = json::object();
    for (std::size_t i = 0; i < static_cast<std::size_t>(Led::Count); ++i) {
        leds[std::string(led_name(static_cast<Led>(i)))] =
            led_color_name(leds_.get(static_cast<Led>(i)));
    }
    j["leds"] = leds;

    json axes = json::object();
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        axes[std::string(axis_name(static_cast<Axis>(i)))] =
            state_.normalized(static_cast<Axis>(i));
    }
    j["axes"] = axes;

    json buttons = json::array();
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        if (state_.buttons[i]) buttons.push_back(i);
    }
    j["buttons"] = buttons;
    j["hat"] = state_.hat;
    j["thumb"] = {state_.thumb_x, state_.thumb_y};

    j["stopwatch"] = {{"running", engine_.stopwatch_running()},
                      {"elapsed_ms", engine_.stopwatch_elapsed_us(now_us()) / 1000}};

    j["profiles"] = cached_profiles_;

    json drift;
    drift["active"] = drift_.active();
    drift["samples"] = drift_.samples();
    json drift_axes = json::object();
    for (Axis axis : {Axis::X, Axis::Y, Axis::Twist, Axis::Throttle}) {
        AxisStats stats = drift_.axis(axis);
        double offset = is_centred_axis(axis) ? stats.offset_from_centre() : 0.0;
        drift_axes[std::string(axis_name(axis))] = {{"offset", offset},
                                                    {"jitter", stats.jitter()},
                                                    {"mean", stats.mean},
                                                    {"min", stats.minimum},
                                                    {"max", stats.maximum}};
    }
    drift["axes"] = drift_axes;
    j["drift"] = drift;
    j["sink"] = {{"joystick", uinput_.has_joystick()},
                 {"keyboard", uinput_.has_keyboard()},
                 {"mouse", uinput_.has_mouse()}};
    return j;
}

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------
void Daemon::reply(Client& c, const json& req, const json& result) {
    json out;
    if (req.contains("id")) out["id"] = req["id"];
    if (result.is_object() && result.contains("error") && result["error"].is_string() &&
        !result["error"].get<std::string>().empty()) {
        out["ok"] = false;
        out["error"] = result["error"];
    } else {
        out["ok"] = true;
        out["result"] = result;
    }
    c.out += out.dump();
    c.out.push_back('\n');
}

void Daemon::reply_error(Client& c, const json& req, const std::string& message) {
    json out;
    if (req.contains("id")) out["id"] = req["id"];
    out["ok"] = false;
    out["error"] = message;
    c.out += out.dump();
    c.out.push_back('\n');
}

void Daemon::broadcast(const json& event, bool stream_only) {
    const std::string line = event.dump() + "\n";
    for (auto& c : clients_) {
        if (!c.subscribed) continue;
        if (stream_only && !c.stream) continue;
        c.out += line;
        flush_client(c);
    }
}

// Compact per-frame payload: raw axis values, button bitmask, hat and thumb.
json Daemon::input_frame() const {
    json d;
    d["a"] = state_.axis;
    uint64_t mask = 0;
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        if (state_.buttons[i]) mask |= (1ull << i);
    }
    d["b"] = mask;
    d["h"] = state_.hat;
    d["tx"] = state_.thumb_x;
    d["ty"] = state_.thumb_y;
    return d;
}

void Daemon::flush_client(Client& c) {
    if (c.fd < 0 || c.out.empty()) return;
    ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
        c.out.erase(0, static_cast<std::size_t>(n));
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ::close(c.fd);
        c.fd = -1;
        c.out.clear();
    }
    // Guard against a stuck client eating memory.
    if (c.out.size() > 1u << 20) {
        ::close(c.fd);
        c.fd = -1;
        c.out.clear();
    }
}

// JSON booleans are strict; accept 0/1 as well so third party clients work.
bool jbool(const json& j, const char* key, bool fallback) {
    if (!j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    return fallback;
}

json Daemon::cmd_set_led(const json& args) {
    if (!args.contains("led") || !args.contains("color")) {
        return {{"error", "set_led needs 'led' and 'color'"}};
    }
    std::string name = args.value("led", std::string("all"));
    LedColor color{};
    if (!led_color_from_name(args.value("color", std::string("off")), color)) {
        return {{"error", "unknown colour"}};
    }
    if (name == "all") {
        for (std::size_t i = 0; i < static_cast<std::size_t>(Led::Count); ++i) {
            leds_.set(static_cast<Led>(i), color);
        }
    } else {
        Led led{};
        if (!led_from_name(name, led)) return {{"error", "unknown LED '" + name + "'"}};
        leds_.set(led, color);
    }
    if (jbool(args, "persist", false)) {
        std::array<LedColor, static_cast<std::size_t>(Led::Count)> c{};
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = leds_.get(static_cast<Led>(i));
        profile_.layer(engine_.mode()).leds = c;
        profile_.layer(engine_.mode()).has_leds = true;
        mark_dirty();
    }
    return {{"ok", true}};
}

json Daemon::cmd_set_brightness(const json& args) {
    if (args.contains("mfd")) {
        int v = std::clamp(args.value("mfd", 128), 0, 255);
        profile_.settings.mfd_brightness = v;
        mfd_.set_brightness(static_cast<uint8_t>(v));
        mark_dirty();
    }
    if (args.contains("led")) {
        int v = std::clamp(args.value("led", 128), 0, 255);
        profile_.settings.led_brightness = v;
        leds_.set_brightness(static_cast<uint8_t>(v));
        mark_dirty();
    }
    return {{"ok", true}};
}

json Daemon::cmd_text(const json& args) {
    if (!args.contains("lines") || !args["lines"].is_array()) {
        return {{"error", "text needs a 'lines' array"}};
    }
    std::array<std::string, kMfdLineCount> lines = profile_.settings.custom_text;
    for (std::size_t i = 0; i < std::min<std::size_t>(kMfdLineCount, args["lines"].size()); ++i) {
        lines[i] = args["lines"][i].get<std::string>();
    }
    profile_.settings.custom_text = lines;
    if (jbool(args, "show", true)) {
        engine_.set_page(MfdPage::Custom);
    }
    mark_dirty();
    next_mfd_us_ = 0;
    return {{"ok", true}};
}

json Daemon::cmd_apply_axis(const json& args) {
    Axis axis{};
    if (!axis_from_name(args.value("axis", std::string()), axis)) {
        return {{"error", "unknown axis"}};
    }
    if (args.contains("center") || args.contains("deadzone") || args.contains("curve") ||
        args.contains("saturation") || args.contains("invert")) {
        AxisConfig& cfg = engine_.profile_mut().axes[static_cast<std::size_t>(axis)];
        if (args.contains("center")) cfg.center = std::clamp(args.value("center", 0.0), -0.4, 0.4);
        if (args.contains("deadzone")) {
            cfg.deadzone = std::clamp(args.value("deadzone", 0.0), 0.0, 0.9);
        }
        if (args.contains("saturation")) {
            cfg.saturation = std::clamp(args.value("saturation", 1.0), 0.1, 1.0);
        }
        if (args.contains("curve")) cfg.curve = std::clamp(args.value("curve", 1.0), 0.1, 4.0);
        if (args.contains("invert")) cfg.invert = jbool(args, "invert", false);
        mark_dirty();
    }
    return {{"ok", true}};
}

json Daemon::telemetry_json() {
    TelemetrySample snap = telemetry_snapshot();
    json j;
    j["source"] = snap.source.empty() ? "none" : snap.source;
    j["connected"] = snap.connected;
    j["valid"] = snap.valid;
    j["error"] = snap.error;
    j["values"] = snap.values;

    json lines = json::array();
    for (std::size_t i = 0; i < kMfdLineCount; ++i) {
        lines.push_back(snap.valid ? format_telemetry_line(profile_.settings.telemetry_lines[i],
                                                           snap.values)
                                   : std::string());
    }
    j["lines"] = lines;
    j["rules"] = static_cast<int>(rules_.size());
    return j;
}

json Daemon::cmd_set_telemetry(const json& args) {
    Profile& p = engine_.profile_mut();

    if (args.contains("source")) {
        std::string name = args.value("source", std::string("none"));
        TelemetrySourceKind kind{};
        if (!telemetry_source_from_name(name, kind)) return {{"error", "unknown source"}};
        p.settings.telemetry_source = telemetry_source_name(kind);
        {
            std::lock_guard<std::mutex> lock(tele_mutex_);
            if (kind != tele_kind_) {
                tele_kind_ = kind;
                telemetry_ = TelemetrySample{};
                telemetry_.source = telemetry_source_name(kind);
            }
        }
    }

    if (args.contains("lines") && args["lines"].is_array()) {
        for (std::size_t i = 0; i < kMfdLineCount && i < args["lines"].size(); ++i) {
            if (args["lines"][i].is_string()) {
                p.settings.telemetry_lines[i] = args["lines"][i].get<std::string>();
            }
        }
    }

    if (args.contains("rules") && args["rules"].is_array()) {
        std::vector<TelemetryRule> rules;
        for (const auto& item : args["rules"]) {
            if (!item.is_object()) continue;
            TelemetryRule rule;
            rule.name = item.value("name", std::string());
            rule.condition = item.value("when", std::string());
            rule.enabled = jbool(item, "enabled", true);
            led_from_name(item.value("led", std::string("fire")), rule.led);
            led_color_from_name(item.value("color", std::string("amber")), rule.color);
            if (!rule.condition.empty()) rules.push_back(std::move(rule));
        }
        rules_ = rules;
        p.rules = rules;
        forced_leds_.clear();
    }

    mark_dirty();
    next_mfd_us_ = 0;
    next_rules_us_ = 0;
    return {{"ok", true}};
}

json Daemon::cmd_settings(const json& args) {
    if (args.contains("autostart")) settings_.autostart = jbool(args, "autostart", settings_.autostart);
    if (args.contains("start_minimized")) {
        settings_.start_minimized = jbool(args, "start_minimized", settings_.start_minimized);
    }
    if (args.contains("verify_link")) {
        settings_.verify_link = jbool(args, "verify_link", settings_.verify_link);
    }
    if (args.contains("autosave")) settings_.autosave = jbool(args, "autosave", settings_.autosave);

    if (args.contains("autostart") || args.contains("start_minimized")) {
        std::string error;
        if (!write_autostart_entry(settings_.autostart, settings_.start_minimized, &error)) {
            return {{"error", "autostart: " + error}};
        }
    }

    std::string error;
    if (!settings_.save(&error)) return {{"error", "settings: " + error}};
    dirty_settings_ = false;
    return json::parse(settings_.to_json_string(0));
}

// Measures the health of the vendor channel: twenty back-to-back control
// transfers. A healthy link answers in well under a millisecond.
json Daemon::cmd_link_test() {
    if (!connected_) return {{"error", "device not connected"}};

    constexpr int kSamples = 20;
    double total = 0.0, worst = 0.0;
    int failures = 0;
    for (int i = 0; i < kSamples; ++i) {
        uint64_t start = now_us();
        DeviceError e = dev_.send(CmdMfdBrightness,
                                  static_cast<uint16_t>(profile_.settings.mfd_brightness));
        uint64_t elapsed = now_us() - start;
        if (e != DeviceError::None) {
            ++failures;
            continue;
        }
        double ms = static_cast<double>(elapsed) / 1000.0;
        total += ms;
        if (ms > worst) worst = ms;
    }
    double avg = (kSamples > failures) ? total / (kSamples - failures) : 0.0;

    // Recent input frame rate, if anything moved in the last two seconds.
    uint64_t t = now_us();
    int frames = 0;
    for (std::size_t i = 0; i < frame_filled_; ++i) {
        uint64_t ts = frame_ring_[(frame_pos_ - 1 - i) % frame_ring_.size()];
        if (t - ts > 2000000ull) break;
        ++frames;
    }

    json j;
    j["samples"] = kSamples;
    j["failures"] = failures;
    j["avg_ms"] = avg;
    j["max_ms"] = worst;
    j["frames_2s"] = frames;
    j["valid"] = failures == 0 && avg < 5.0;
    j["note"] = failures ? "control transfers failed"
                          : (avg < 5.0 ? "vendor channel healthy" : "slow control transfers");
    return j;
}

json Daemon::diagnostics_json() const {
    json checks = json::array();
    auto add = [&checks](const Check& c) {
        checks.push_back({{"name", c.name},
                          {"level", check_level_name(c.level)},
                          {"detail", c.detail}});
    };

    add(connected_ ? Check::ok("Device", "06a3:0762 open")
                   : Check::fail("Device", last_error_.empty() ? "not connected" : last_error_));
    add(Device::has_write_access()
            ? Check::ok("USB permissions", "device node is writable")
            : Check::fail("USB permissions", "install the udev rule and replug"));
    add(input_.fd() >= 0
            ? Check::ok("Input endpoint", input_.path())
            : Check::fail("Input endpoint", "hidraw and evdev both unavailable"));
    add(uinput_.has_joystick()
            ? Check::ok("Virtual devices", "joystick, keyboard and mouse created")
            : Check::fail("Virtual devices", uinput_.error().empty() ? "uinput unavailable"
                                                                     : uinput_.error()));
    add(grabbed_ ? Check::ok("Exclusive grab", "physical node is captured")
                 : Check::warn("Exclusive grab", "not captured: games may see double input"));

    if (drift_.samples() > 0) {
        for (Axis axis : {Axis::X, Axis::Y, Axis::Twist}) {
            add(classify_centre(axis, drift_.axis(axis)));
        }
    } else {
        add(Check::warn("Axis rest position", "run a drift test to measure it"));
    }

    uint64_t now = now_us();
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        if (button_since_[i] && now - button_since_[i] > 8000000ull) {
            add(Check::warn(std::string(button_name(static_cast<Button>(i))),
                            "held down for more than 8 seconds"));
        }
    }

    add(connected_ ? Check::ok("Clock", "re-programmed every 5 seconds")
                   : Check::warn("Clock", "cannot update while the device is offline"));

    json out;
    out["checks"] = checks;
    out["drift_samples"] = drift_.samples();
    return out;
}

json Daemon::cmd_profile(const json& args) {
    std::string action = args.value("action", std::string("get"));

    if (action == "list") {
        return {{"profiles", list_profiles()}, {"current", profile_name_}};
    }
    if (action == "get") {
        return {{"name", profile_name_}, {"profile", json::parse(profile_.to_json_string(0))}};
    }
    if (action == "load") {
        std::string name = args.value("name", std::string());
        if (name.empty()) return {{"error", "load needs a profile name"}};
        std::string path = profile_path_for(name);
        Profile p;
        std::string error;
        if (!Profile::load(path, p, error)) return {{"error", error}};
        apply_profile(p, true);
        engine_.set_mode(1);
        return {{"ok", true}, {"profile", name}};
    }
    if (action == "save") {
        std::string name = args.value("name", profile_name_);
        profile_.name = name;
        std::string error;
        if (!profile_.save(profile_path_for(name), error)) return {{"error", error}};
        profile_name_ = name;
        dirty_profile_ = false;
        return {{"ok", true}, {"profile", name}};
    }
    if (action == "create") {
        Profile p = Profile::make_default();
        p.name = args.value("name", std::string("New profile"));
        if (args.contains("from")) {
            std::string base = args.value("from", std::string());
            std::string error;
            Profile loaded;
            if (!Profile::load(profile_path_for(base), loaded, error)) return {{"error", error}};
            p = loaded;
            p.name = args.value("name", loaded.name + " copy");
        }
        apply_profile(p, true);
        dirty_profile_ = true;
        return {{"ok", true}, {"profile", p.name}};
    }
    if (action == "set") {
        if (!args.contains("profile")) return {{"error", "set needs a profile object"}};
        Profile p;
        std::string error;
        if (!Profile::from_json_string(args["profile"].dump(), p, error)) {
            return {{"error", error}};
        }
        apply_profile(p, true);
        mark_dirty();
        return {{"ok", true}};
    }
    if (action == "delete") {
        std::string name = args.value("name", std::string());
        if (name.empty()) return {{"error", "delete needs a name"}};
        if (::unlink(profile_path_for(name).c_str()) != 0) {
            return {{"error", std::string("cannot delete: ") + std::strerror(errno)}};
        }
        return {{"ok", true}};
    }
    return {{"error", "unknown profile action '" + action + "'"}};
}

void Daemon::handle_request(Client& c, const json& req) {
    std::string cmd = req.value("cmd", std::string());
    json args = req.value("args", json::object());

    if (cmd == "get_state" || cmd == "state") {
        reply(c, req, state_json());
    } else if (cmd == "subscribe") {
        c.subscribed = true;
        c.stream = jbool(args, "stream", false);
        reply(c, req, {{"ok", true}});
        json ev{{"event", "state"}, {"data", state_json()}};
        c.out += ev.dump() + "\n";
    } else if (cmd == "set_mode") {
        engine_.set_mode(args.value("mode", 1));
        apply_mode(true);
        reply(c, req, {{"mode", engine_.mode()}});
    } else if (cmd == "set_page") {
        MfdPage page{};
        if (!mfd_page_from_name(args.value("page", std::string("status")), page)) {
            reply_error(c, req, "unknown page");
            return;
        }
        engine_.set_page(page);
        next_mfd_us_ = 0;
        reply(c, req, {{"page", mfd_page_name(page)}});
    } else if (cmd == "set_text") {
        reply(c, req, cmd_text(args));
    } else if (cmd == "set_led") {
        reply(c, req, cmd_set_led(args));
    } else if (cmd == "set_brightness") {
        reply(c, req, cmd_set_brightness(args));
    } else if (cmd == "set_shift") {
        engine_.set_shift(jbool(args, "on", false));
        leds_.set_shift(engine_.shift_active());
        reply(c, req, {{"shift", engine_.shift_active()}});
    } else if (cmd == "stopwatch") {
        std::string op = args.value("op", std::string("start"));
        uint64_t t = now_us();
        if (op == "start") engine_.stopwatch_start(t);
        else if (op == "stop") engine_.stopwatch_stop(t);
        else if (op == "reset") engine_.stopwatch_reset();
        reply(c, req, {{"running", engine_.stopwatch_running()},
                       {"elapsed_ms", engine_.stopwatch_elapsed_us(t) / 1000}});
    } else if (cmd == "set_clock") {
        if (args.contains("h24")) profile_.settings.clock_24h = jbool(args, "h24", true);
        if (args.contains("clock2_offset")) {
            profile_.settings.clock2_offset = args.value("clock2_offset", 0);
        }
        if (args.contains("clock3_offset")) {
            profile_.settings.clock3_offset = args.value("clock3_offset", 0);
        }
        sync_clock();
        reply(c, req, {{"ok", true}});
    } else if (cmd == "clock_now") {
        clock_.set_from_system(true);
        reply(c, req, {{"ok", true}});
    } else if (cmd == "leds_refresh") {
        apply_leds();
        reply(c, req, {{"ok", true}});
    } else if (cmd == "get_settings") {
        reply(c, req, json::parse(settings_.to_json_string(0)));
    } else if (cmd == "set_settings") {
        reply(c, req, cmd_settings(args));
    } else if (cmd == "telemetry") {
        reply(c, req, telemetry_json());
    } else if (cmd == "set_telemetry") {
        reply(c, req, cmd_set_telemetry(args));
    } else if (cmd == "link_test") {
        reply(c, req, cmd_link_test());
    } else if (cmd == "drift") {
        std::string op = args.value("op", std::string("start"));
        if (op == "start") drift_.start();
        else if (op == "stop") drift_.stop();
        else drift_.reset();
        reply(c, req, {{"active", drift_.active()}, {"samples", drift_.samples()}});
    } else if (cmd == "apply_axis") {
        reply(c, req, cmd_apply_axis(args));
    } else if (cmd == "diagnostics") {
        reply(c, req, diagnostics_json());
    } else if (cmd == "profile") {
        reply(c, req, cmd_profile(args));
    } else if (cmd == "raw") {
        if (!connected_) {
            reply_error(c, req, "device not connected");
            return;
        }
        uint16_t command = static_cast<uint16_t>(args.value("command", 0));
        uint16_t value = static_cast<uint16_t>(args.value("value", 0));
        DeviceError e = dev_.send(command, value);
        reply(c, req, {{"error", device_error_name(e)}, {"message", dev_.error_string()}});
    } else if (cmd == "reconnect") {
        disconnect("manual reconnect");
        next_reconnect_us_ = 0;
        reply(c, req, {{"ok", true}});
    } else if (cmd == "shutdown") {
        reply(c, req, {{"ok", true}});
        flush_client(c);
        ::exit(0);
    } else {
        reply_error(c, req, "unknown command '" + cmd + "'");
    }
}

void Daemon::handle_client(Client& c) {
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            c.in.append(buf, static_cast<std::size_t>(n));
            if (c.in.size() > 1u << 20) {
                ::close(c.fd);
                c.fd = -1;
                return;
            }
            continue;
        }
        if (n == 0) {
            ::close(c.fd);
            c.fd = -1;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        ::close(c.fd);
        c.fd = -1;
        return;
    }

    std::size_t pos;
    while ((pos = c.in.find('\n')) != std::string::npos) {
        std::string line = c.in.substr(0, pos);
        c.in.erase(0, pos + 1);
        if (line.empty()) continue;
        json req;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            json err{{"ok", false}, {"error", std::string("bad json: ") + e.what()}};
            c.out += err.dump() + "\n";
            continue;
        }
        try {
            handle_request(c, req);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "x52d: request '%s' failed: %s\n",
                         req.value("cmd", std::string("?")).c_str(), e.what());
            reply_error(c, req, std::string("internal error: ") + e.what());
        }
    }
    flush_client(c);
}

void Daemon::accept_clients() {
    for (;;) {
        int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        Client c;
        c.fd = fd;
        clients_.push_back(std::move(c));
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
int Daemon::run() {
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGCHLD, SIG_IGN);

    sock_path_ = socket_path();
    ::unlink(sock_path_.c_str());
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "x52d: socket: %s\n", std::strerror(errno));
        return 1;
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path_.c_str());
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "x52d: bind %s: %s\n", sock_path_.c_str(), std::strerror(errno));
        return 1;
    }
    ::chmod(sock_path_.c_str(), 0600);
    ::listen(listen_fd_, 8);

    // Virtual devices.
    if (!uinput_.create_joystick("X52 Pro (x52d)")) {
        std::fprintf(stderr, "x52d: %s\n", uinput_.error().c_str());
    }
    if (!uinput_.create_keyboard("X52 Pro Keyboard (x52d)")) {
        std::fprintf(stderr, "x52d: %s\n", uinput_.error().c_str());
    }
    if (!uinput_.create_mouse("X52 Pro Mouse (x52d)")) {
        std::fprintf(stderr, "x52d: %s\n", uinput_.error().c_str());
    }

    engine_.on_mode_changed = [this](int mode) {
        apply_mode(true);
        settings_.last_mode = mode;
        dirty_settings_ = true;
        last_change_us_ = now_us();
    };
    engine_.on_shift_changed = [this](bool on) { leds_.set_shift(on); };
    engine_.on_page_changed = [this](MfdPage page) {
        next_mfd_us_ = 0;
        settings_.last_page = mfd_page_name(page);
        dirty_settings_ = true;
        last_change_us_ = now_us();
    };

    // Restore the last session: settings tell us which profile, mode and page
    // were active when the daemon last stopped.
    {
        std::string load_error;
        if (!settings_.load(&load_error)) {
            std::fprintf(stderr, "x52d: settings: %s (writing defaults)\n", load_error.c_str());
            settings_.save();
        } else {
            std::fprintf(stderr, "x52d: settings: restored profile=%s mode=%d page=%s\n",
                         settings_.last_profile.c_str(), settings_.last_mode,
                         settings_.last_page.c_str());
        }
    }
    dirty_settings_ = false;

    Profile initial;
    std::string err;
    bool loaded = false;
    if (!settings_.last_profile.empty()) {
        loaded = Profile::load(profile_path_for(settings_.last_profile), initial, err);
    }
    if (!loaded) loaded = Profile::load(profile_path_for("Default"), initial, err);
    if (!loaded) {
        initial = Profile::make_example();
    }
    // Applying a profile fires the mode/page callbacks, which write the
    // session state; capture the restored values first so they survive.
    const int want_mode = settings_.last_mode;
    const std::string want_page = settings_.last_page;

    profile_name_ = initial.name;
    apply_profile(initial, true);

    engine_.set_mode(want_mode);
    MfdPage start_page = MfdPage::Status;
    if (mfd_page_from_name(want_page, start_page)) engine_.set_page(start_page);

    // The callbacks already wrote these, but make it explicit: a profile whose
    // own page matches the restored page fires no callback at all.
    settings_.last_mode = want_mode;
    settings_.last_page = want_page;
    dirty_settings_ = false;

    std::fprintf(stderr, "x52d: session restored: mode=%d page=%s profile=%s\n", engine_.mode(),
                 mfd_page_name(engine_.page()), profile_name_.c_str());

    cached_profiles_ = list_profiles();
    tele_thread_ = std::thread([this] {
        std::unique_ptr<TelemetrySource> source;
        TelemetrySourceKind active = TelemetrySourceKind::None;
        while (!tele_stop_) {
            TelemetrySourceKind kind;
            {
                std::lock_guard<std::mutex> lock(tele_mutex_);
                kind = tele_kind_;
            }
            if (kind != active) {
                source = make_telemetry_source(kind);
                active = kind;
                std::lock_guard<std::mutex> lock(tele_mutex_);
                telemetry_ = TelemetrySample{};
                telemetry_.source = telemetry_source_name(kind);
            }
            if (source) {
                TelemetryValues values;
                std::string error;
                bool ok = source->poll(values, error);
                std::lock_guard<std::mutex> lock(tele_mutex_);
                telemetry_.valid = ok;
                telemetry_.connected = ok;
                telemetry_.error = ok ? std::string() : error;
                if (ok) telemetry_.values = std::move(values);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });
    tele_thread_.detach();

    if (!try_connect()) next_reconnect_us_ = now_us() + kReconnectUs;

    std::printf("x52d: listening on %s\n", sock_path_.c_str());
    std::fflush(stdout);

    for (;;) {
        uint64_t t = now_us();

        // Reconnect / hotplug.
        if (!connected_ && t >= next_reconnect_us_) {
            if (!try_connect()) {
                next_reconnect_us_ = t + kReconnectUs;
            } else {
                std::printf("x52d: device connected\n");
                std::fflush(stdout);
            }
        }

        // Input.
        bool input_changed = false;
        if (connected_) {
            bool got = false;
            for (int i = 0; i < 32; ++i) {
                int rc = input_.poll(state_);
                if (rc <= 0) break;
                got = true;
            }
            if (got) {
                frame_ring_[frame_pos_ % frame_ring_.size()] = t;
                ++frame_pos_;
                if (frame_filled_ < frame_ring_.size()) ++frame_filled_;
                drift_.add(state_);
                for (std::size_t i = 0; i < kButtonCount; ++i) {
                    if (state_.buttons[i]) {
                        if (button_since_[i] == 0) button_since_[i] = t;
                    } else {
                        button_since_[i] = 0;
                    }
                }
                engine_.set_now(t);
                engine_.process(state_, prev_);
                prev_ = state_;
                input_changed = true;
                uinput_.sync_joystick();
                if (profile_.settings.grab_device && !grabbed_) {
                    grabbed_ = input_.set_grab(true);
                } else if (!profile_.settings.grab_device && grabbed_) {
                    input_.set_grab(false);
                    grabbed_ = false;
                }
            }
            engine_.tick(t);

            if (t >= next_clock_us_) {
                sync_clock();  // the device has no RTC tick of its own
                next_clock_us_ = t + 5000000;
            }

            if (t >= next_mfd_us_) {
                auto lines = render_lines(t);
                mfd_.set_line(0, lines[0]);
                mfd_.set_line(1, lines[1]);
                mfd_.set_line(2, lines[2]);
                mfd_.set_brightness(static_cast<uint8_t>(profile_.settings.mfd_brightness));
                leds_.set_brightness(static_cast<uint8_t>(profile_.settings.led_brightness));
                flush_outputs();
                next_mfd_us_ = t + kMfdRefreshUs;
            }
        }

        if (t >= next_rules_us_ && !rules_.empty()) apply_rules(t);

        if (input_changed && t - last_input_push_us_ >= kInputPushUs) {
            json frame{{"event", "input"}, {"data", input_frame()}};
            broadcast(frame, true);
            last_input_push_us_ = t;
        }

        // Persist after a short quiet period so a burst of edits costs one write.
        if (t - last_change_us_ > 1200000ull && (dirty_profile_ || dirty_settings_)) {
            std::string save_error;
            if (dirty_profile_ && settings_.autosave) {
                profile_.name = profile_name_;
                if (!profile_.save(profile_path_for(profile_name_), save_error)) {
                    std::fprintf(stderr, "x52d: autosave: %s\n", save_error.c_str());
                }
            }
            settings_.last_profile = profile_name_;
            settings_.save(&save_error);
            cached_profiles_ = list_profiles();
            dirty_profile_ = false;
            dirty_settings_ = false;
        }

        if (t >= next_profiles_us_) {
            cached_profiles_ = list_profiles();
            next_profiles_us_ = t + 10000000;
        }

        // Push state to subscribers.
        if (t >= next_push_us_) {
            bool any = false;
            for (const auto& c : clients_) any = any || c.subscribed;
            if (any) {
                json ev{{"event", "state"}, {"data", state_json()}};
                broadcast(ev);
            }
            next_push_us_ = t + kStatePushUs;
        }

        // poll
        std::vector<struct pollfd> fds;
        fds.push_back({listen_fd_, POLLIN, 0});
        for (const auto& c : clients_) {
            if (c.fd >= 0) fds.push_back({c.fd, POLLIN, 0});
        }
        std::size_t input_index = static_cast<std::size_t>(-1);
        if (connected_ && input_.fd() >= 0) {
            input_index = fds.size();
            fds.push_back({input_.fd(), POLLIN, 0});
        }

        int rc = ::poll(fds.data(), fds.size(), kTickMs);
        if (rc > 0) {
            if (fds[0].revents & POLLIN) accept_clients();
            std::size_t idx = 1;
            for (auto& c : clients_) {
                if (c.fd < 0) continue;
                if (idx < fds.size() && (fds[idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                    handle_client(c);
                }
                ++idx;
            }
        }

        // Reap closed clients.
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                      [](const Client& c) { return c.fd < 0; }),
                       clients_.end());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool foreground = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::printf("x52d - X52 Pro daemon\n\nUsage: x52d [--daemon]\n"
                        "  --daemon   detach from the terminal\n");
            return 0;
        }
        if (a == "--daemon") foreground = false;
    }
    if (!foreground) {
        if (::daemon(0, 0) != 0) {
            std::perror("daemon");
            return 1;
        }
    }
    Daemon d;
    return d.run();
}
