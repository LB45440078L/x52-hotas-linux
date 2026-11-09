// SPDX-License-Identifier: BSD-3-Clause
#include "x52/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace x52 {
namespace {

struct NameEntry {
    std::string_view name;
};

constexpr std::array<std::string_view, kButtonCount> kButtonNames = {
    "trigger",        // 0
    "fire",           // 1
    "a",              // 2
    "b",              // 3
    "c",              // 4
    "shift",          // 5
    "d",              // 6
    "e",              // 7
    "t1-up",          // 8
    "t1-down",        // 9
    "t2-up",          // 10
    "t2-down",        // 11
    "t3-up",          // 12
    "t3-down",        // 13
    "secondary-trigger",  // 14
    "mouse-primary",  // 15
    "wheel-down",     // 16
    "wheel-up",       // 17
    "mouse-secondary",// 18
    "hat-up",         // 19
    "hat-right",      // 20
    "hat-down",       // 21
    "hat-left",       // 22
    "throttle-hat-up",    // 23
    "throttle-hat-right", // 24
    "throttle-hat-down",  // 25
    "throttle-hat-left",  // 26
    "mode-1",         // 27
    "mode-2",         // 28
    "mode-3",         // 29
    "clutch",         // 30
    "function",       // 31
    "start-stop",     // 32
    "reset",          // 33
    "page-up",        // 34
    "page-down",      // 35
    "up",             // 36
    "down",           // 37
    "mfd-select",     // 38
};

constexpr std::array<std::string_view, kAxisCount> kAxisNames = {
    "x", "y", "twist", "throttle", "rotary-x", "rotary-y", "slider",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Led::Count)> kLedNames = {
    "fire", "a", "b", "d", "e", "t1", "t2", "t3", "pov", "clutch", "throttle",
};

bool equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

}  // namespace

LedWiring led_wiring(Led led) noexcept {
    switch (led) {
        case Led::Fire:     return {LedId::Fire, LedId::Fire, false};
        case Led::A:        return {LedId::ARed, LedId::AGreen, true};
        case Led::B:        return {LedId::BRed, LedId::BGreen, true};
        case Led::D:        return {LedId::DRed, LedId::DGreen, true};
        case Led::E:        return {LedId::ERed, LedId::EGreen, true};
        case Led::T1:       return {LedId::T1Red, LedId::T1Green, true};
        case Led::T2:       return {LedId::T2Red, LedId::T2Green, true};
        case Led::T3:       return {LedId::T3Red, LedId::T3Green, true};
        case Led::Pov:      return {LedId::PovRed, LedId::PovGreen, true};
        case Led::Clutch:   return {LedId::IRed, LedId::IGreen, true};
        case Led::Throttle: return {LedId::Throttle, LedId::Throttle, false};
        case Led::Count:    break;
    }
    return {LedId::Fire, LedId::Fire, false};
}

std::string_view led_name(Led led) noexcept {
    auto i = static_cast<std::size_t>(led);
    if (i >= kLedNames.size()) return "?";
    return kLedNames[i];
}

bool led_from_name(std::string_view name, Led& out) noexcept {
    for (std::size_t i = 0; i < kLedNames.size(); ++i) {
        if (equals_ci(name, kLedNames[i])) {
            out = static_cast<Led>(i);
            return true;
        }
    }
    // A couple of friendly aliases used by the vendor software.
    if (equals_ci(name, "i")) { out = Led::Clutch; return true; }
    if (equals_ci(name, "trigger")) { out = Led::Fire; return true; }
    return false;
}

std::string_view led_color_name(LedColor c) noexcept {
    switch (c) {
        case LedColor::Off:   return "off";
        case LedColor::Red:   return "red";
        case LedColor::Amber: return "amber";
        case LedColor::Green: return "green";
    }
    return "off";
}

bool led_color_from_name(std::string_view name, LedColor& out) noexcept {
    if (equals_ci(name, "off") || equals_ci(name, "none")) { out = LedColor::Off; return true; }
    if (equals_ci(name, "red")) { out = LedColor::Red; return true; }
    if (equals_ci(name, "amber") || equals_ci(name, "yellow") || equals_ci(name, "orange")) {
        out = LedColor::Amber; return true;
    }
    if (equals_ci(name, "green")) { out = LedColor::Green; return true; }
    return false;
}

std::string_view button_name(Button b) noexcept {
    auto i = static_cast<std::size_t>(b);
    if (i >= kButtonNames.size()) return "?";
    return kButtonNames[i];
}

bool button_from_name(std::string_view name, Button& out) noexcept {
    for (std::size_t i = 0; i < kButtonNames.size(); ++i) {
        if (equals_ci(name, kButtonNames[i])) {
            out = static_cast<Button>(i);
            return true;
        }
    }
    return false;
}

std::string_view axis_name(Axis a) noexcept {
    auto i = static_cast<std::size_t>(a);
    if (i >= kAxisNames.size()) return "?";
    return kAxisNames[i];
}

bool axis_from_name(std::string_view name, Axis& out) noexcept {
    for (std::size_t i = 0; i < kAxisNames.size(); ++i) {
        if (equals_ci(name, kAxisNames[i])) {
            out = static_cast<Axis>(i);
            return true;
        }
    }
    return false;
}

}  // namespace x52
