// SPDX-License-Identifier: BSD-3-Clause
#include "x52/leds.hpp"

namespace x52 {

Leds::Leds() { reset(); }

void Leds::reset() {
    colors_.fill(LedColor::Off);
    physical_.fill(false);
    physical_dirty_.fill(false);
    brightness_ = 128;
    brightness_dirty_ = false;
    shift_ = false;
    shift_dirty_ = false;
    blink_ = false;
    blink_dirty_ = false;
    mark_all_dirty();
}

void Leds::apply(Led led, LedColor color) {
    LedWiring w = led_wiring(led);
    auto red = static_cast<std::size_t>(w.red);
    auto green = static_cast<std::size_t>(w.green);

    if (!w.bicolour) {
        bool on = color != LedColor::Off;
        physical_[red] = on;
        physical_dirty_[red] = true;
        return;
    }

    bool r = (color == LedColor::Red || color == LedColor::Amber);
    bool g = (color == LedColor::Green || color == LedColor::Amber);
    physical_[red] = r;
    physical_[green] = g;
    physical_dirty_[red] = true;
    physical_dirty_[green] = true;
}

void Leds::set(Led led, LedColor color) {
    auto i = static_cast<std::size_t>(led);
    if (i >= colors_.size()) return;
    if (colors_[i] == color) return;
    colors_[i] = color;
    apply(led, color);
}

LedColor Leds::get(Led led) const {
    auto i = static_cast<std::size_t>(led);
    return i < colors_.size() ? colors_[i] : LedColor::Off;
}

void Leds::set_all(LedColor color) {
    for (std::size_t i = 0; i < colors_.size(); ++i) {
        set(static_cast<Led>(i), color);
    }
}

void Leds::set_brightness(uint8_t value) {
    if (brightness_ == value) return;
    brightness_ = value;
    brightness_dirty_ = true;
}

void Leds::set_shift(bool on) {
    if (shift_ == on) return;
    shift_ = on;
    shift_dirty_ = true;
}

void Leds::set_blink(bool on) {
    if (blink_ == on) return;
    blink_ = on;
    blink_dirty_ = true;
}

void Leds::mark_all_dirty() {
    physical_dirty_.fill(true);
    brightness_dirty_ = true;
    shift_dirty_ = true;
    blink_dirty_ = true;
}

void Leds::mark_all_clean() {
    physical_dirty_.fill(false);
    brightness_dirty_ = false;
    shift_dirty_ = false;
    blink_dirty_ = false;
}

DeviceError Leds::flush(Transport& dev) {
    if (!dev.is_open()) return DeviceError::Disconnected;

    if (brightness_dirty_) {
        DeviceError e = dev.send(CmdLedBrightness, brightness_);
        if (e != DeviceError::None) return e;
        brightness_dirty_ = false;
    }

    for (std::size_t id = 1; id < physical_.size(); ++id) {
        if (!physical_dirty_[id]) continue;
        uint16_t value = static_cast<uint16_t>((id << 8) | (physical_[id] ? 1 : 0));
        DeviceError e = dev.send(CmdLed, value);
        if (e != DeviceError::None) return e;
        physical_dirty_[id] = false;
    }

    if (shift_dirty_) {
        DeviceError e = dev.send(CmdShiftIndicator, shift_ ? kIndicatorOn : kIndicatorOff);
        if (e != DeviceError::None) return e;
        shift_dirty_ = false;
    }

    if (blink_dirty_) {
        DeviceError e = dev.send(CmdBlinkIndicator, blink_ ? kIndicatorOn : kIndicatorOff);
        if (e != DeviceError::None) return e;
        blink_dirty_ = false;
    }

    return DeviceError::None;
}

}  // namespace x52
