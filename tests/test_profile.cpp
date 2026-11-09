// SPDX-License-Identifier: BSD-3-Clause
#include "test_framework.hpp"

#include <string>

#include "x52/profile.hpp"

using namespace x52;

TEST(profile_default_is_all_passthrough) {
    Profile p = Profile::make_default();
    CHECK_EQ(p.name, std::string("Default"));
    for (int m = 0; m < kModeCount; ++m) {
        for (std::size_t b = 0; b < kButtonCount; ++b) {
            CHECK(p.modes[m].buttons[b].type == ActionType::Passthrough);
        }
    }
    CHECK_EQ(p.validate(), std::string());
}

TEST(profile_round_trip_keeps_actions) {
    Profile p = Profile::make_example();
    std::string err;
    Profile q;
    CHECK(Profile::from_json_string(p.to_json_string(), q, err));
    CHECK_EQ(err, std::string());
    CHECK_EQ(q.name, p.name);
    CHECK_EQ(q.description, p.description);

    CHECK(q.modes[1].buttons[1].type == ActionType::Macro);
    CHECK_EQ(q.modes[1].buttons[1].macro.size(), 3u);
    CHECK_EQ(q.modes[1].buttons[1].macro[0], std::string("text:hello world"));

    CHECK(q.modes[0].has_leds);
    CHECK(q.modes[0].leds[static_cast<std::size_t>(Led::T1)] == LedColor::Green);
    CHECK(q.modes[1].leds[static_cast<std::size_t>(Led::T1)] == LedColor::Red);
}

TEST(profile_round_trip_keeps_settings) {
    Profile p = Profile::make_default();
    p.settings.led_brightness = 42;
    p.settings.mfd_brightness = 7;
    p.settings.mfd_page = MfdPage::Custom;
    p.settings.clock2_offset = -120;
    p.settings.custom_text = {"ONE", "TWO", "THREE"};

    Profile q;
    std::string err;
    CHECK(Profile::from_json_string(p.to_json_string(), q, err));
    CHECK_EQ(q.settings.led_brightness, 42);
    CHECK_EQ(q.settings.mfd_brightness, 7);
    CHECK(q.settings.mfd_page == MfdPage::Custom);
    CHECK_EQ(q.settings.clock2_offset, -120);
    CHECK_EQ(q.settings.custom_text[1], std::string("TWO"));
}

TEST(profile_rejects_invalid_json) {
    Profile q;
    std::string err;
    CHECK(!Profile::from_json_string("{not json", q, err));
    CHECK(!err.empty());
}

TEST(profile_rejects_newer_schema) {
    Profile q;
    std::string err;
    CHECK(!Profile::from_json_string(R"({"schema": 2, "name": "x"})", q, err));
    CHECK(err.find("schema") != std::string::npos);
}

TEST(profile_accepts_shorthand_actions) {
    Profile q;
    std::string err;
    bool ok = Profile::from_json_string(
        R"({"name":"short","modes":{"1":{"buttons":{"1":"key:space","2":"none"}}}})", q, err);
    CHECK(ok);
    CHECK(q.modes[0].buttons[1].type == ActionType::Key);
    CHECK_EQ(q.modes[0].buttons[1].key, std::string("space"));
    CHECK(q.modes[0].buttons[2].type == ActionType::None);
}

TEST(profile_clamps_axis_config) {
    Profile q;
    std::string err;
    CHECK(Profile::from_json_string(
        R"({"name":"ax","axes":{"x":{"deadzone":5.0,"saturation":0.0,"curve":99}}})", q, err));
    CHECK(q.axes[0].deadzone <= 0.9);
    CHECK(q.axes[0].saturation >= 0.1);
    CHECK(q.axes[0].curve <= 4.0);
}

TEST(profile_validation_flags_empty_key) {
    Profile p = Profile::make_default();
    p.modes[0].buttons[0].type = ActionType::Key;
    p.modes[0].buttons[0].key.clear();
    CHECK(!p.validate().empty());
}

TEST(profile_path_is_sanitised) {
    std::string path = profile_path_for("Elite Dangerous/../x");
    CHECK(path.find("..") == std::string::npos);
    CHECK(path.find("Elite Dangerous") != std::string::npos);
    CHECK(path.size() > 5);
    CHECK(path.substr(path.size() - 5) == ".json");
}

TEST(shape_axis_identity) {
    AxisConfig cfg;
    CHECK_NEAR(shape_axis(0.0, cfg), 0.0, 1e-9);
    CHECK_NEAR(shape_axis(0.5, cfg), 0.5, 1e-9);
    CHECK_NEAR(shape_axis(1.0, cfg), 1.0, 1e-9);
}

TEST(shape_axis_deadzone_snaps_to_centre) {
    AxisConfig cfg;
    cfg.deadzone = 0.1;
    CHECK_NEAR(shape_axis(0.52, cfg), 0.5, 1e-9);
    CHECK_NEAR(shape_axis(0.48, cfg), 0.5, 1e-9);
    CHECK(shape_axis(0.9, cfg) > 0.5);
}

TEST(shape_axis_invert) {
    AxisConfig cfg;
    cfg.invert = true;
    CHECK_NEAR(shape_axis(0.0, cfg), 1.0, 1e-9);
    CHECK_NEAR(shape_axis(1.0, cfg), 0.0, 1e-9);
}

TEST(shape_axis_curve_monotonic) {
    AxisConfig cfg;
    cfg.curve = 2.0;
    double a = shape_axis(0.7, cfg);
    double b = shape_axis(0.8, cfg);
    CHECK(b > a);
    CHECK(b <= 1.0);
}
