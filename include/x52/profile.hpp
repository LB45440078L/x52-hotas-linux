// SPDX-License-Identifier: GPL-2.0-or-later
//
// Profile model: modes, per-mode LED themes, MFD pages, button actions and
// axis shaping.  Profiles are plain JSON so they are easy to version, diff
// and share.
//
#ifndef X52_PROFILE_HPP
#define X52_PROFILE_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "x52/protocol.hpp"

namespace x52 {

inline constexpr int kModeCount = 3;

enum class MfdPage : uint8_t { Clock = 0, Stopwatch, Status, Custom, Profile };
const char* mfd_page_name(MfdPage p) noexcept;
bool mfd_page_from_name(std::string_view name, MfdPage& out) noexcept;

enum class MouseButton : uint8_t { Left = 0, Right = 1, Middle = 2, Side = 3, Extra = 4 };
const char* mouse_button_name(MouseButton b) noexcept;
bool mouse_button_from_name(std::string_view name, MouseButton& out) noexcept;

enum class ActionType : uint8_t {
    None = 0,   // button is swallowed
    Passthrough,// forwarded to the virtual joystick unchanged
    Key,        // keyboard key
    Mouse,      // mouse button
    Wheel,      // mouse wheel
    Macro,      // sequence of keys / text / delays
    Mode,       // switch active mode
    MfdPage,    // switch MFD page
    Exec,       // run a shell command
    Button,     // emulate another joystick button
};

struct Action {
    ActionType type = ActionType::None;

    std::string key;                 // Key
    std::vector<std::string> macro;  // Macro steps
    MouseButton mouse = MouseButton::Left;
    int wheel_delta = 0;             // positive = up
    int mode = 1;                    // Mode, 1-based
    MfdPage page = MfdPage::Status;  // MfdPage
    std::string command;             // Exec
    int button = -1;                 // Button

    bool operator==(const Action& o) const;
    static Action passthrough() { Action a; a.type = ActionType::Passthrough; return a; }
    static Action none() { Action a; a.type = ActionType::None; return a; }
};

struct AxisConfig {
    double deadzone = 0.0;    // 0..0.9, fraction of full travel
    double saturation = 1.0;  // 0.1..1, fraction where the axis hits full scale
    double curve = 1.0;       // >1 = softer centre, <1 = sharper centre
    bool invert = false;

    bool operator==(const AxisConfig& o) const;
};

struct ModeLayer {
    std::string name;
    std::array<Action, kButtonCount> buttons{};
    std::array<Action, kButtonCount> shift_buttons{};
    std::array<LedColor, static_cast<std::size_t>(Led::Count)> leds{};
    bool has_leds = false;
    std::array<std::string, kMfdLineCount> mfd_lines{};
    bool has_mfd = false;
};

struct Settings {
    int led_brightness = 128;   // 0..255
    int mfd_brightness = 128;   // 0..255
    MfdPage mfd_page = MfdPage::Status;
    bool clock_24h = true;
    bool clock_show_date = true;  // alternate date and time on the clock page
    int clock2_offset = 0;        // minutes from clock 1
    int clock3_offset = 0;
    bool grab_device = true;      // exclusively capture the physical device
    bool mode_indicators = true;  // recolour LEDs when the mode changes
    bool shift_on_pinky = true;   // pinky switch acts as a shift layer
    bool clutch_as_shift = false; // hold clutch for the shift layer as well
    std::string mfd_clock_label = "X52 PRO";
    std::array<std::string, kMfdLineCount> custom_text{{"X52 PRO", "LINUX DRIVER", ""}};
};

struct Profile {
    std::string name = "Default";
    std::string description;
    std::string author;
    Settings settings;
    std::array<AxisConfig, kAxisCount> axes{};
    std::array<ModeLayer, kModeCount> modes{};

    static Profile make_default();
    // A profile with the vendor-style defaults (modes 1-3 with LED colours).
    static Profile make_example();

    std::string validate() const;  // empty string when valid

    std::string to_json_string(int indent = 2) const;
    static bool from_json_string(std::string_view text, Profile& out, std::string& error);

    bool save(const std::string& path, std::string& error) const;
    static bool load(const std::string& path, Profile& out, std::string& error);

    // Mode helpers (1-based mode numbers).
    const ModeLayer& layer(int mode) const;
    ModeLayer& layer(int mode);
};

// Directory where profiles live: $XDG_CONFIG_HOME/x52pro/profiles
std::string profile_directory();
std::vector<std::string> list_profiles();
std::string profile_path_for(const std::string& name);

// Applies deadzone/saturation/curve to a normalised 0..1 axis value.
double shape_axis(double value, const AxisConfig& cfg) noexcept;

}  // namespace x52

#endif  // X52_PROFILE_HPP
