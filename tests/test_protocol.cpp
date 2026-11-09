// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_framework.hpp"

#include <string>

#include "x52/protocol.hpp"

using namespace x52;

TEST(led_names_round_trip) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(Led::Count); ++i) {
        Led parsed{};
        CHECK(led_from_name(led_name(static_cast<Led>(i)), parsed));
        CHECK_EQ(static_cast<int>(parsed), static_cast<int>(i));
    }
}

TEST(led_aliases) {
    Led l{};
    CHECK(led_from_name("I", l));
    CHECK(l == Led::Clutch);
    CHECK(led_from_name("trigger", l));
    CHECK(l == Led::Fire);
    CHECK(!led_from_name("nope", l));
}

TEST(led_wiring_ids_match_protocol) {
    // The firmware addresses red/green halves individually; the ids must match
    // the documented LED map or colours come out wrong.
    CHECK_EQ(static_cast<int>(led_wiring(Led::A).red), 2);
    CHECK_EQ(static_cast<int>(led_wiring(Led::A).green), 3);
    CHECK_EQ(static_cast<int>(led_wiring(Led::Clutch).red), 18);
    CHECK_EQ(static_cast<int>(led_wiring(Led::Clutch).green), 19);
    CHECK_EQ(static_cast<int>(led_wiring(Led::Throttle).red), 20);
    CHECK(!led_wiring(Led::Throttle).bicolour);
    CHECK(led_wiring(Led::T3).bicolour);
}

TEST(button_names_round_trip) {
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        Button b{};
        CHECK(button_from_name(button_name(static_cast<Button>(i)), b));
        CHECK_EQ(static_cast<int>(b), static_cast<int>(i));
    }
    CHECK_EQ(std::string(button_name(Button::MfdSelect)), std::string("mfd-select"));
}

TEST(axis_names_round_trip) {
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        Axis a{};
        CHECK(axis_from_name(axis_name(static_cast<Axis>(i)), a));
        CHECK_EQ(static_cast<int>(a), static_cast<int>(i));
    }
}

TEST(led_colors_parse) {
    LedColor c{};
    CHECK(led_color_from_name("amber", c));
    CHECK(c == LedColor::Amber);
    CHECK(led_color_from_name("YELLOW", c));
    CHECK(c == LedColor::Amber);
    CHECK(!led_color_from_name("blue", c));
}

TEST(protocol_constants) {
    CHECK_EQ(CmdMfdLine1, 0xd1);
    CHECK_EQ(CmdMfdLine2, 0xd2);
    CHECK_EQ(CmdMfdLine3, 0xd4);
    CHECK_EQ(CmdMfdLine1 | CmdMfdClearMask, 0xd9);
    CHECK_EQ(CmdMfdLine2 | CmdMfdClearMask, 0xda);
    CHECK_EQ(CmdMfdLine3 | CmdMfdClearMask, 0xdc);
    CHECK_EQ(kControlRequest, 0x91);
    CHECK_EQ(kControlRequestType, 0x40);
}
