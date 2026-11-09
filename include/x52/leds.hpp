// SPDX-License-Identifier: GPL-2.0-or-later
//
// LED and indicator model: bicolour LEDs, brightness, shift and blink.
//
#ifndef X52_LEDS_HPP
#define X52_LEDS_HPP

#include <array>
#include <cstdint>

#include "x52/device.hpp"
#include "x52/protocol.hpp"

namespace x52 {

class Leds {
public:
    Leds();

    void set(Led led, LedColor color);
    LedColor get(Led led) const;

    void set_brightness(uint8_t value);
    uint8_t brightness() const noexcept { return brightness_; }

    void set_shift(bool on);
    bool shift() const noexcept { return shift_; }

    void set_blink(bool on);
    bool blink() const noexcept { return blink_; }

    void set_all(LedColor color);
    void reset();

    void mark_all_dirty();
    void mark_all_clean();

    DeviceError flush(Transport& dev);

private:
    std::array<LedColor, static_cast<std::size_t>(Led::Count)> colors_{};
    // Physical LED id -> on/off, indexed by LedId value.
    std::array<bool, 21> physical_{};
    std::array<bool, 21> physical_dirty_{};

    uint8_t brightness_ = 128;
    bool brightness_dirty_ = false;
    bool shift_ = false;
    bool shift_dirty_ = false;
    bool blink_ = false;
    bool blink_dirty_ = false;

    void apply(Led led, LedColor color);
};

}  // namespace x52

#endif  // X52_LEDS_HPP
