// SPDX-License-Identifier: BSD-3-Clause
#include "test_framework.hpp"

#include "test_support.hpp"
#include "x52/clock.hpp"
#include "x52/leds.hpp"
#include "x52/protocol.hpp"

using namespace x52;

TEST(clock_packs_hour_minute) {
    test::RecordingTransport t;
    Clock c;
    c.set_time(13, 45, ClockFormat::H24);
    CHECK(c.flush(t) == DeviceError::None);
    CHECK(t.contains(CmdClock1, 0x0d2d));
}

TEST(clock_12h_sets_high_bit) {
    test::RecordingTransport t;
    Clock c;
    c.set_time(1, 2, ClockFormat::H12);
    c.flush(t);
    CHECK(t.contains(CmdClock1, 0x8102));
}

TEST(clock_date_packing) {
    test::RecordingTransport t;
    Clock c;
    c.set_date(9, 11, 26);
    c.flush(t);
    CHECK(t.contains(CmdDateDayMonth, 0x0b09));  // month << 8 | day
    CHECK(t.contains(CmdDateYear, 26));
}

TEST(clock_offset_negative) {
    test::RecordingTransport t;
    Clock c;
    c.set_offset(1, -60, ClockFormat::H24);
    c.flush(t);
    CHECK(t.contains(CmdClock2Offset, 0x0400 | 60));
}

TEST(clock_offset_positive_clock3) {
    test::RecordingTransport t;
    Clock c;
    c.set_offset(2, 330, ClockFormat::H24);  // +05:30
    c.flush(t);
    CHECK(t.contains(CmdClock3Offset, 330));
}

TEST(clock_offset_out_of_range_wraps) {
    test::RecordingTransport t;
    Clock c;
    c.set_offset(1, 1400, ClockFormat::H24);  // 23h20m -> -40m, sign flipped
    c.flush(t);
    CHECK(t.contains(CmdClock2Offset, 0x0400 | 40));
}

TEST(clock_clamps_time) {
    Clock c;
    c.set_time(25, 61);
    CHECK_EQ(c.hour(), 1);
    CHECK_EQ(c.minute(), 1);
}

TEST(leds_bicolour_maps_to_two_physical) {
    test::RecordingTransport t;
    Leds leds;
    leds.mark_all_clean();
    leds.set(Led::A, LedColor::Amber);
    leds.flush(t);

    // LED id 2 (red) and 3 (green), both on.
    CHECK(t.contains(CmdLed, (2 << 8) | 1));
    CHECK(t.contains(CmdLed, (3 << 8) | 1));
}

TEST(leds_red_only) {
    test::RecordingTransport t;
    Leds leds;
    leds.mark_all_clean();
    leds.set(Led::T1, LedColor::Red);
    leds.flush(t);
    CHECK(t.contains(CmdLed, (10 << 8) | 1));
    CHECK(t.contains(CmdLed, (11 << 8) | 0));
}

TEST(leds_single_colour_throttle) {
    test::RecordingTransport t;
    Leds leds;
    leds.mark_all_clean();
    leds.set(Led::Throttle, LedColor::Amber);  // not supported: treated as "on"
    leds.flush(t);
    CHECK(t.contains(CmdLed, (20 << 8) | 1));
}

TEST(leds_shift_and_blink) {
    test::RecordingTransport t;
    Leds leds;
    leds.mark_all_clean();
    leds.set_shift(true);
    leds.set_blink(true);
    leds.flush(t);
    CHECK(t.contains(CmdShiftIndicator, kIndicatorOn));
    CHECK(t.contains(CmdBlinkIndicator, kIndicatorOn));
}

TEST(leds_no_redundant_traffic) {
    test::RecordingTransport t;
    Leds leds;
    leds.mark_all_clean();
    leds.set(Led::B, LedColor::Off);  // already off
    leds.flush(t);
    CHECK_EQ(t.commands.size(), 0u);
}
