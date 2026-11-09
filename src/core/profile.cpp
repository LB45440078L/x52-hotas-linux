// SPDX-License-Identifier: BSD-3-Clause
#include "x52/profile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

json led_color_to_json(LedColor c) { return std::string(led_color_name(c)); }

bool led_color_from_json(const json& j, LedColor& out) {
    if (!j.is_string()) return false;
    return led_color_from_name(j.get<std::string>(), out);
}

json action_to_json(const Action& a) {
    json j;
    switch (a.type) {
        case ActionType::None: j["action"] = "none"; break;
        case ActionType::Passthrough: j["action"] = "passthrough"; break;
        case ActionType::Key:
            j["action"] = "key";
            j["key"] = a.key;
            break;
        case ActionType::Mouse:
            j["action"] = "mouse";
            j["button"] = mouse_button_name(a.mouse);
            break;
        case ActionType::Wheel:
            j["action"] = "wheel";
            j["delta"] = a.wheel_delta;
            break;
        case ActionType::Macro:
            j["action"] = "macro";
            j["sequence"] = a.macro;
            break;
        case ActionType::Mode:
            j["action"] = "mode";
            j["mode"] = a.mode;
            break;
        case ActionType::MfdPage:
            j["action"] = "mfd_page";
            j["page"] = mfd_page_name(a.page);
            break;
        case ActionType::Exec:
            j["action"] = "exec";
            j["command"] = a.command;
            break;
        case ActionType::Button:
            j["action"] = "button";
            j["button"] = a.button;
            break;
    }
    return j;
}

Action action_from_json(const json& j, std::string& warn) {
    Action a;
    if (j.is_string()) {
        // Shorthand: "key:space", "none", "passthrough"
        std::string s = j.get<std::string>();
        auto colon = s.find(':');
        std::string kind = lower(colon == std::string::npos ? s : s.substr(0, colon));
        std::string arg = colon == std::string::npos ? std::string() : s.substr(colon + 1);
        if (kind == "key") { a.type = ActionType::Key; a.key = arg; }
        else if (kind == "none") { a.type = ActionType::None; }
        else if (kind == "passthrough") { a.type = ActionType::Passthrough; }
        else warn = "unknown shorthand action '" + s + "'";
        return a;
    }
    if (!j.is_object()) {
        warn = "action must be an object";
        return a;
    }

    std::string kind = lower(j.value("action", std::string("none")));
    if (kind == "none") a.type = ActionType::None;
    else if (kind == "passthrough") a.type = ActionType::Passthrough;
    else if (kind == "key") { a.type = ActionType::Key; a.key = j.value("key", std::string()); }
    else if (kind == "mouse") {
        a.type = ActionType::Mouse;
        mouse_button_from_name(j.value("button", std::string("left")), a.mouse);
    } else if (kind == "wheel") {
        a.type = ActionType::Wheel;
        a.wheel_delta = j.value("delta", 1);
    } else if (kind == "macro") {
        a.type = ActionType::Macro;
        if (j.contains("sequence") && j["sequence"].is_array()) {
            for (const auto& s : j["sequence"]) {
                if (s.is_string()) a.macro.push_back(s.get<std::string>());
            }
        }
    } else if (kind == "mode") {
        a.type = ActionType::Mode;
        a.mode = std::clamp(j.value("mode", 1), 1, kModeCount);
    } else if (kind == "mfd_page") {
        a.type = ActionType::MfdPage;
        mfd_page_from_name(j.value("page", std::string("status")), a.page);
    } else if (kind == "exec") {
        a.type = ActionType::Exec;
        a.command = j.value("command", std::string());
    } else if (kind == "button") {
        a.type = ActionType::Button;
        a.button = j.value("button", -1);
    } else {
        warn = "unknown action type '" + kind + "'";
    }
    return a;
}

json mode_to_json(const ModeLayer& m) {
    json j;
    j["name"] = m.name;
    if (m.has_leds) {
        json leds = json::object();
        for (std::size_t i = 0; i < m.leds.size(); ++i) {
            leds[std::string(led_name(static_cast<Led>(i)))] = led_color_to_json(m.leds[i]);
        }
        j["leds"] = leds;
    }
    if (m.has_mfd) j["mfd"] = m.mfd_lines;

    json buttons = json::object();
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        if (m.buttons[i].type == ActionType::Passthrough) continue;
        buttons[std::to_string(i)] = action_to_json(m.buttons[i]);
    }
    if (!buttons.empty()) j["buttons"] = buttons;

    json shift = json::object();
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        if (m.shift_buttons[i].type == ActionType::Passthrough) continue;
        shift[std::to_string(i)] = action_to_json(m.shift_buttons[i]);
    }
    if (!shift.empty()) j["shift_buttons"] = shift;
    return j;
}

void mode_from_json(const json& j, ModeLayer& m, std::string& warn) {
    if (j.contains("name")) m.name = j.value("name", std::string());
    if (j.contains("leds") && j["leds"].is_object()) {
        for (auto it = j["leds"].begin(); it != j["leds"].end(); ++it) {
            Led led{};
            LedColor color{};
            if (led_from_name(it.key(), led) && led_color_from_json(it.value(), color)) {
                m.leds[static_cast<std::size_t>(led)] = color;
                m.has_leds = true;
            }
        }
    }
    if (j.contains("mfd") && j["mfd"].is_array()) {
        for (std::size_t i = 0; i < std::min<std::size_t>(kMfdLineCount, j["mfd"].size()); ++i) {
            if (j["mfd"][i].is_string()) m.mfd_lines[i] = j["mfd"][i].get<std::string>();
        }
        m.has_mfd = true;
    }
    auto read_buttons = [&](const json& src, std::array<Action, kButtonCount>& dst) {
        for (auto it = src.begin(); it != src.end(); ++it) {
            int idx = -1;
            try { idx = std::stoi(it.key()); } catch (...) { continue; }
            if (idx < 0 || idx >= static_cast<int>(kButtonCount)) continue;
            dst[static_cast<std::size_t>(idx)] = action_from_json(it.value(), warn);
        }
    };
    if (j.contains("buttons") && j["buttons"].is_object()) read_buttons(j["buttons"], m.buttons);
    if (j.contains("shift_buttons") && j["shift_buttons"].is_object()) {
        read_buttons(j["shift_buttons"], m.shift_buttons);
    }
}

}  // namespace

const char* mfd_page_name(MfdPage p) noexcept {
    switch (p) {
        case MfdPage::Clock: return "clock";
        case MfdPage::Stopwatch: return "stopwatch";
        case MfdPage::Status: return "status";
        case MfdPage::Custom: return "custom";
        case MfdPage::Profile: return "profile";
    }
    return "status";
}

bool mfd_page_from_name(std::string_view name, MfdPage& out) noexcept {
    std::string s = lower(std::string(name));
    if (s == "clock" || s == "time") { out = MfdPage::Clock; return true; }
    if (s == "stopwatch" || s == "timer") { out = MfdPage::Stopwatch; return true; }
    if (s == "status" || s == "info") { out = MfdPage::Status; return true; }
    if (s == "custom" || s == "text") { out = MfdPage::Custom; return true; }
    if (s == "profile") { out = MfdPage::Profile; return true; }
    return false;
}

const char* mouse_button_name(MouseButton b) noexcept {
    switch (b) {
        case MouseButton::Left: return "left";
        case MouseButton::Right: return "right";
        case MouseButton::Middle: return "middle";
        case MouseButton::Side: return "side";
        case MouseButton::Extra: return "extra";
    }
    return "left";
}

bool mouse_button_from_name(std::string_view name, MouseButton& out) noexcept {
    std::string s = lower(std::string(name));
    if (s == "left" || s == "1") { out = MouseButton::Left; return true; }
    if (s == "right" || s == "2") { out = MouseButton::Right; return true; }
    if (s == "middle" || s == "3") { out = MouseButton::Middle; return true; }
    if (s == "side" || s == "4") { out = MouseButton::Side; return true; }
    if (s == "extra" || s == "5") { out = MouseButton::Extra; return true; }
    return false;
}

bool Action::operator==(const Action& o) const {
    return type == o.type && key == o.key && macro == o.macro && mouse == o.mouse &&
           wheel_delta == o.wheel_delta && mode == o.mode && page == o.page &&
           command == o.command && button == o.button;
}

bool AxisConfig::operator==(const AxisConfig& o) const {
    return center == o.center && deadzone == o.deadzone && saturation == o.saturation &&
           curve == o.curve && invert == o.invert;
}

double shape_axis(double value, const AxisConfig& cfg) noexcept {
    // Trim first: it corrects the mechanical rest position of the axis, so the
    // deadzone and the curve are centred on the real zero.
    value = std::clamp(value - cfg.center, 0.0, 1.0);
    if (cfg.invert) value = 1.0 - value;

    // Treat the axis as a signed stick: centre at 0.5 for the two "centred"
    // axes; for throttle-like axes the whole range is used.
    double dz = std::clamp(cfg.deadzone, 0.0, 0.9);
    double sat = std::clamp(cfg.saturation, 0.1, 1.0);

    double centred = value * 2.0 - 1.0;
    double mag = std::fabs(centred);
    if (mag <= dz) return 0.5;

    mag = (mag - dz) / std::max(1e-6, (sat - dz));
    mag = std::clamp(mag, 0.0, 1.0);
    if (cfg.curve != 1.0) mag = std::pow(mag, cfg.curve);

    return (centred < 0 ? -1.0 : 1.0) * mag * 0.5 + 0.5;
}

Profile Profile::make_default() {
    Profile p;
    p.name = "Default";
    p.description = "Passthrough profile: every button and axis behaves like a plain joystick.";
    for (auto& m : p.modes) {
        m.buttons.fill(Action::passthrough());
        m.shift_buttons.fill(Action::passthrough());
    }
    return p;
}

Profile Profile::make_example() {
    Profile p = make_default();
    p.name = "Demo";
    p.description = "Shows off modes, LED themes, a macro and MFD pages.";

    p.modes[0].name = "Navigation";
    p.modes[1].name = "Combat";
    p.modes[2].name = "Docking";

    const LedColor nav[] = {LedColor::Amber, LedColor::Green, LedColor::Green, LedColor::Off,
                           LedColor::Green, LedColor::Green, LedColor::Green, LedColor::Off,
                           LedColor::Green, LedColor::Off, LedColor::Amber};
    const LedColor combat[] = {LedColor::Red, LedColor::Red, LedColor::Amber, LedColor::Off,
                              LedColor::Red, LedColor::Red, LedColor::Amber, LedColor::Off,
                              LedColor::Red, LedColor::Amber, LedColor::Red};
    const LedColor dock[] = {LedColor::Green, LedColor::Amber, LedColor::Amber, LedColor::Off,
                            LedColor::Amber, LedColor::Amber, LedColor::Green, LedColor::Off,
                            LedColor::Amber, LedColor::Green, LedColor::Green};
    for (std::size_t i = 0; i < std::size_t(Led::Count); ++i) {
        p.modes[0].leds[i] = nav[i];
        p.modes[1].leds[i] = combat[i];
        p.modes[2].leds[i] = dock[i];
    }
    for (auto& m : p.modes) m.has_leds = true;

    p.modes[0].mfd_lines = {"NAVIGATION", "MODE 1", ""};
    p.modes[1].mfd_lines = {"COMBAT", "MODE 2", ""};
    p.modes[2].mfd_lines = {"DOCKING", "MODE 3", ""};
    for (auto& m : p.modes) m.has_mfd = true;

    Action macro;
    macro.type = ActionType::Macro;
    macro.macro = {"text:hello world", "delay:60", "enter"};
    p.modes[1].buttons[1] = macro;  // FIRE launches a typed message

    Action page;
    page.type = ActionType::MfdPage;
    page.page = MfdPage::Clock;
    p.modes[0].buttons[38] = page;  // MFD SELECT cycles page (see engine)

    return p;
}

std::string Profile::validate() const {
    if (name.empty()) return "profile name must not be empty";
    if (settings.led_brightness < 0 || settings.led_brightness > 255) {
        return "led_brightness out of range";
    }
    if (settings.mfd_brightness < 0 || settings.mfd_brightness > 255) {
        return "mfd_brightness out of range";
    }
    for (int m = 0; m < kModeCount; ++m) {
        for (std::size_t b = 0; b < kButtonCount; ++b) {
            const Action& a = modes[m].buttons[b];
            if (a.type == ActionType::Key && a.key.empty()) {
                return "mode " + std::to_string(m + 1) + " button " + std::to_string(b) +
                       ": key action without a key";
            }
            if (a.type == ActionType::Button && (a.button < 0 || a.button >= int(kButtonCount))) {
                return "mode " + std::to_string(m + 1) + " button " + std::to_string(b) +
                       ": invalid target button";
            }
            if (a.type == ActionType::Exec && a.command.empty()) {
                return "mode " + std::to_string(m + 1) + " button " + std::to_string(b) +
                       ": empty command";
            }
        }
    }
    return {};
}

const ModeLayer& Profile::layer(int mode) const {
    int idx = std::clamp(mode - 1, 0, kModeCount - 1);
    return modes[static_cast<std::size_t>(idx)];
}

ModeLayer& Profile::layer(int mode) {
    int idx = std::clamp(mode - 1, 0, kModeCount - 1);
    return modes[static_cast<std::size_t>(idx)];
}

std::string Profile::to_json_string(int indent) const {
    json j;
    j["schema"] = 1;
    j["name"] = name;
    if (!description.empty()) j["description"] = description;
    if (!author.empty()) j["author"] = author;

    json jsettings;
    jsettings["led_brightness"] = settings.led_brightness;
    jsettings["mfd_brightness"] = settings.mfd_brightness;
    jsettings["mfd_page"] = mfd_page_name(settings.mfd_page);
    jsettings["clock_24h"] = settings.clock_24h;
    jsettings["clock_show_date"] = settings.clock_show_date;
    jsettings["clock2_offset"] = settings.clock2_offset;
    jsettings["clock3_offset"] = settings.clock3_offset;
    jsettings["grab_device"] = settings.grab_device;
    jsettings["mode_indicators"] = settings.mode_indicators;
    jsettings["shift_on_pinky"] = settings.shift_on_pinky;
    jsettings["clutch_as_shift"] = settings.clutch_as_shift;
    jsettings["mfd_clock_label"] = settings.mfd_clock_label;
    jsettings["custom_text"] = settings.custom_text;
    j["settings"] = jsettings;

    json jaxes = json::object();
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        json a;
        a["center"] = axes[i].center;
        a["deadzone"] = axes[i].deadzone;
        a["saturation"] = axes[i].saturation;
        a["curve"] = axes[i].curve;
        a["invert"] = axes[i].invert;
        if (a != json{{"center", 0.0}, {"deadzone", 0.0}, {"saturation", 1.0},
                      {"curve", 1.0}, {"invert", false}}) {
            jaxes[std::string(axis_name(static_cast<Axis>(i)))] = a;
        }
    }
    if (!jaxes.empty()) j["axes"] = jaxes;

    json jmodes = json::object();
    for (int m = 0; m < kModeCount; ++m) {
        jmodes[std::to_string(m + 1)] = mode_to_json(modes[static_cast<std::size_t>(m)]);
    }
    j["modes"] = jmodes;
    return j.dump(indent);
}

bool Profile::from_json_string(std::string_view text, Profile& out, std::string& error) {
    json j;
    try {
        j = json::parse(text.begin(), text.end());
    } catch (const std::exception& e) {
        error = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "profile root must be a JSON object";
        return false;
    }

    Profile p = Profile::make_default();
    p.name = j.value("name", std::string("Untitled"));
    p.description = j.value("description", std::string());
    p.author = j.value("author", std::string());

    int schema = j.value("schema", 1);
    if (schema > 1) {
        error = "profile schema " + std::to_string(schema) + " is newer than supported (1)";
        return false;
    }

    if (j.contains("settings") && j["settings"].is_object()) {
        const json& s = j["settings"];
        p.settings.led_brightness = std::clamp(s.value("led_brightness", 128), 0, 255);
        p.settings.mfd_brightness = std::clamp(s.value("mfd_brightness", 128), 0, 255);
        if (s.contains("mfd_page")) {
            mfd_page_from_name(s.value("mfd_page", std::string("status")), p.settings.mfd_page);
        }
        p.settings.clock_24h = s.value("clock_24h", true);
        p.settings.clock_show_date = s.value("clock_show_date", true);
        p.settings.clock2_offset = std::clamp(s.value("clock2_offset", 0), -1439, 1439);
        p.settings.clock3_offset = std::clamp(s.value("clock3_offset", 0), -1439, 1439);
        p.settings.grab_device = s.value("grab_device", true);
        p.settings.mode_indicators = s.value("mode_indicators", true);
        p.settings.shift_on_pinky = s.value("shift_on_pinky", true);
        p.settings.clutch_as_shift = s.value("clutch_as_shift", false);
        p.settings.mfd_clock_label = s.value("mfd_clock_label", std::string("X52 PRO"));
        if (s.contains("custom_text") && s["custom_text"].is_array()) {
            for (std::size_t i = 0; i < std::min<std::size_t>(kMfdLineCount, s["custom_text"].size()); ++i) {
                if (s["custom_text"][i].is_string()) {
                    p.settings.custom_text[i] = s["custom_text"][i].get<std::string>();
                }
            }
        }
    }

    if (j.contains("axes") && j["axes"].is_object()) {
        for (auto it = j["axes"].begin(); it != j["axes"].end(); ++it) {
            Axis a{};
            if (!axis_from_name(it.key(), a)) continue;
            const json& v = it.value();
            AxisConfig cfg;
            cfg.center = std::clamp(v.value("center", 0.0), -0.4, 0.4);
            cfg.deadzone = std::clamp(v.value("deadzone", 0.0), 0.0, 0.9);
            cfg.saturation = std::clamp(v.value("saturation", 1.0), 0.1, 1.0);
            cfg.curve = std::clamp(v.value("curve", 1.0), 0.1, 4.0);
            cfg.invert = v.value("invert", false);
            p.axes[static_cast<std::size_t>(a)] = cfg;
        }
    }

    if (j.contains("modes") && j["modes"].is_object()) {
        std::string warn;
        for (auto it = j["modes"].begin(); it != j["modes"].end(); ++it) {
            int idx = -1;
            try { idx = std::stoi(it.key()); } catch (...) { continue; }
            if (idx < 1 || idx > kModeCount || !it.value().is_object()) continue;
            mode_from_json(it.value(), p.modes[static_cast<std::size_t>(idx - 1)], warn);
        }
    }

    std::string err = p.validate();
    if (!err.empty()) {
        error = err;
        return false;
    }
    out = std::move(p);
    return true;
}

bool Profile::save(const std::string& path, std::string& error) const {
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    f << to_json_string(2) << '\n';
    if (!f) {
        error = "failed writing " + path;
        return false;
    }
    return true;
}

bool Profile::load(const std::string& path, Profile& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot read " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return from_json_string(ss.str(), out, error);
}

std::string profile_directory() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = home ? std::filesystem::path(home) / ".config" : std::filesystem::path(".");
    }
    return (base / "x52pro" / "profiles").string();
}

std::vector<std::string> list_profiles() {
    std::vector<std::string> names;
    std::error_code ec;
    std::filesystem::path dir(profile_directory());
    if (!std::filesystem::exists(dir, ec)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        names.push_back(entry.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string profile_path_for(const std::string& name) {
    std::string safe;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') {
            safe.push_back(c);
        } else {
            safe.push_back('_');
        }
    }
    if (safe.empty()) safe = "profile";
    return (std::filesystem::path(profile_directory()) / (safe + ".json")).string();
}

}  // namespace x52
