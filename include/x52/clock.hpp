// SPDX-License-Identifier: GPL-2.0-or-later
//
// On-board clock / date programming.  The X52 Pro keeps its own clock, which
// means the MFD keeps showing the correct time even when no software is
// running.  Clocks 2 and 3 are stored as minute offsets from clock 1.
//
#ifndef X52_CLOCK_HPP
#define X52_CLOCK_HPP

#include <cstdint>

#include "x52/device.hpp"

namespace x52 {

enum class ClockFormat : uint8_t { H24 = 0, H12 = 1 };

class Clock {
public:
    // Base clock (the one shown by default).
    void set_time(int hour, int minute, ClockFormat fmt = ClockFormat::H24);
    // Offset clocks: index 1 -> clock 2, index 2 -> clock 3.
    void set_offset(int index, int minutes, ClockFormat fmt = ClockFormat::H24);
    void set_date(int day, int month, int year2digit);

    // Programs the base clock and date from the host clock.
    void set_from_system(bool local_time = true);

    int hour() const noexcept { return hour_; }
    int minute() const noexcept { return minute_; }
    int day() const noexcept { return day_; }
    int month() const noexcept { return month_; }
    int year() const noexcept { return year_; }
    int offset(int index) const noexcept;
    ClockFormat format(int index) const noexcept;

    void mark_dirty();
    DeviceError flush(Transport& dev);

private:
    int hour_ = 0, minute_ = 0;
    int day_ = 1, month_ = 1, year_ = 0;
    ClockFormat fmt_[3] = {ClockFormat::H24, ClockFormat::H24, ClockFormat::H24};
    int offset_[3] = {0, 0, 0};

    bool time_dirty_ = false;
    bool date_dirty_ = false;
    bool offset_dirty_[2] = {false, false};
};

}  // namespace x52

#endif  // X52_CLOCK_HPP
