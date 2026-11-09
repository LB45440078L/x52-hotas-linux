// SPDX-License-Identifier: BSD-3-Clause
#include "x52/clock.hpp"

#include <ctime>

#include "x52/protocol.hpp"

namespace x52 {

void Clock::set_time(int hour, int minute, ClockFormat fmt) {
    hour = ((hour % 24) + 24) % 24;
    minute = ((minute % 60) + 60) % 60;
    if (hour == hour_ && minute == minute_ && fmt == fmt_[0]) return;
    hour_ = hour;
    minute_ = minute;
    fmt_[0] = fmt;
    time_dirty_ = true;
}

void Clock::set_offset(int index, int minutes, ClockFormat fmt) {
    if (index < 1 || index > 2) return;
    if (minutes < -1439) minutes = -1439;
    if (minutes > 1439) minutes = 1439;
    if (offset_[index] == minutes && fmt_[index] == fmt) return;
    offset_[index] = minutes;
    fmt_[index] = fmt;
    offset_dirty_[index - 1] = true;
}

void Clock::set_date(int day, int month, int year2digit) {
    day = day < 1 ? 1 : (day > 31 ? 31 : day);
    month = month < 1 ? 1 : (month > 12 ? 12 : month);
    year2digit = ((year2digit % 100) + 100) % 100;
    if (day == day_ && month == month_ && year2digit == year_) return;
    day_ = day;
    month_ = month;
    year_ = year2digit;
    date_dirty_ = true;
}

void Clock::set_from_system(bool local_time) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    if (local_time) {
        localtime_r(&now, &tm);
    } else {
        gmtime_r(&now, &tm);
    }
    set_date(tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    set_time(tm.tm_hour, tm.tm_min, ClockFormat::H24);

    if (local_time) {
        long gmtoff = tm.tm_gmtoff >= 0 ? tm.tm_gmtoff : 0;
        (void)gmtoff;  // the base clock is local time; offsets are relative
    }
}

int Clock::offset(int index) const noexcept {
    if (index < 0 || index > 2) return 0;
    return offset_[index];
}

ClockFormat Clock::format(int index) const noexcept {
    if (index < 0 || index > 2) return ClockFormat::H24;
    return fmt_[index];
}

void Clock::mark_dirty() {
    time_dirty_ = true;
    date_dirty_ = true;
    offset_dirty_[0] = true;
    offset_dirty_[1] = true;
}

DeviceError Clock::flush(Transport& dev) {
    if (!dev.is_open()) return DeviceError::Disconnected;

    if (date_dirty_) {
        uint16_t dm = static_cast<uint16_t>((month_ << 8) | day_);
        DeviceError e = dev.send(CmdDateDayMonth, dm);
        if (e != DeviceError::None) return e;
        e = dev.send(CmdDateYear, static_cast<uint16_t>(year_));
        if (e != DeviceError::None) return e;
        date_dirty_ = false;
    }

    if (time_dirty_) {
        uint16_t value = static_cast<uint16_t>((fmt_[0] == ClockFormat::H12 ? 0x8000 : 0) |
                                               ((hour_ & 0x7f) << 8) | (minute_ & 0xff));
        DeviceError e = dev.send(CmdClock1, value);
        if (e != DeviceError::None) return e;
        time_dirty_ = false;
    }

    for (int idx = 1; idx <= 2; ++idx) {
        if (!offset_dirty_[idx - 1]) continue;
        int off = offset_[idx];
        bool negative = off < 0;
        int magnitude = negative ? -off : off;

        // The packet only carries 10 bits; fold out-of-range offsets by a
        // whole day, flipping the sign when needed.
        while (magnitude > 1023) {
            magnitude -= 1440;
            if (magnitude < 0) {
                magnitude = -magnitude;
                negative = !negative;
            }
        }

        uint16_t value = static_cast<uint16_t>((fmt_[idx] == ClockFormat::H12 ? 0x8000 : 0) |
                                               (negative ? 0x0400 : 0) |
                                               (magnitude & 0x03ff));
        DeviceError e = dev.send(idx == 1 ? CmdClock2Offset : CmdClock3Offset, value);
        if (e != DeviceError::None) return e;
        offset_dirty_[idx - 1] = false;
    }

    return DeviceError::None;
}

}  // namespace x52
