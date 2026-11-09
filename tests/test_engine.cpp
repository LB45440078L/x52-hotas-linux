// SPDX-License-Identifier: BSD-3-Clause
#include "test_framework.hpp"

#include <linux/input.h>

#include <string>
#include <vector>

#include "test_support.hpp"
#include "x52/engine.hpp"
#include "x52/profile.hpp"

using namespace x52;

namespace {

InputState with_buttons(std::initializer_list<Button> pressed) {
    InputState st;
    for (Button b : pressed) st.buttons[static_cast<std::size_t>(b)] = true;
    return st;
}

}  // namespace

TEST(engine_passthrough_forwards_buttons) {
    test::RecordingSink sink;
    Engine e(&sink);
    e.set_profile(Profile::make_default());

    InputState empty;
    InputState down = with_buttons({Button::A});
    e.process(down, empty);
    CHECK(sink.has_button(2, true));

    sink.clear();
    e.process(empty, down);
    CHECK(sink.has_button(2, false));
}

TEST(engine_key_action_emits_keycode) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[0].buttons[1].type = ActionType::Key;
    p.modes[0].buttons[1].key = "space";
    e.set_profile(p);

    InputState empty;
    e.process(with_buttons({Button::Fire}), empty);
    CHECK(sink.has_key(KEY_SPACE, true));

    sink.clear();
    e.process(empty, with_buttons({Button::Fire}));
    CHECK(sink.has_key(KEY_SPACE, false));
}

TEST(engine_swallows_none) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[0].buttons[4].type = ActionType::None;
    e.set_profile(p);

    InputState empty;
    e.process(with_buttons({Button::C}), empty);
    CHECK(sink.buttons.empty());
}

TEST(engine_mode_button_switches_mode) {
    test::RecordingSink sink;
    Engine e(&sink);
    e.set_profile(Profile::make_default());
    CHECK_EQ(e.mode(), 1);

    InputState empty;
    e.process(with_buttons({Button::Mode2}), empty);
    CHECK_EQ(e.mode(), 2);

    e.process(with_buttons({Button::Mode3}), empty);
    CHECK_EQ(e.mode(), 3);
}

TEST(engine_mode_uses_its_own_layer) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[1].buttons[2].type = ActionType::Key;  // mode 2, button A
    p.modes[1].buttons[2].key = "b";
    e.set_profile(p);

    InputState empty;
    e.process(with_buttons({Button::A}), empty);
    CHECK(sink.buttons.size() == 1);  // mode 1 -> passthrough

    sink.clear();
    e.process(with_buttons({Button::Mode2}), empty);
    sink.clear();
    e.process(with_buttons({Button::Mode2, Button::A}), empty);
    CHECK(sink.has_key(KEY_B, true));
}

TEST(engine_shift_layer_overrides) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[0].shift_buttons[2].type = ActionType::Key;  // shift + A
    p.modes[0].shift_buttons[2].key = "enter";
    e.set_profile(p);

    InputState empty;
    InputState shift_down = with_buttons({Button::Shift});
    e.process(shift_down, empty);
    CHECK(e.shift_active());

    e.process(with_buttons({Button::Shift, Button::A}), shift_down);
    CHECK(sink.has_key(KEY_ENTER, true));
}

TEST(engine_unbound_shift_falls_back_to_base) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[0].buttons[3].type = ActionType::Key;
    p.modes[0].buttons[3].key = "f1";
    e.set_profile(p);

    InputState empty;
    InputState shift_down = with_buttons({Button::Shift});
    e.process(shift_down, empty);
    e.process(with_buttons({Button::Shift, Button::B}), shift_down);
    CHECK(sink.has_key(KEY_F1, true));
}

TEST(engine_wheel_and_exec_actions) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[0].buttons[6].type = ActionType::Wheel;
    p.modes[0].buttons[6].wheel_delta = -1;
    p.modes[0].buttons[7].type = ActionType::Exec;
    p.modes[0].buttons[7].command = "true";
    e.set_profile(p);

    InputState empty;
    e.process(with_buttons({Button::D, Button::E}), empty);
    CHECK_EQ(sink.wheels.size(), 1u);
    CHECK_EQ(sink.wheels[0], -1);
    CHECK_EQ(sink.execs.size(), 1u);
    CHECK_EQ(sink.execs[0], std::string("true"));
}

TEST(engine_mouse_action) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[0].buttons[2].type = ActionType::Mouse;
    p.modes[0].buttons[2].mouse = MouseButton::Right;
    e.set_profile(p);

    InputState empty;
    e.process(with_buttons({Button::A}), empty);
    CHECK_EQ(sink.mouse.size(), 1u);
    CHECK_EQ(static_cast<int>(sink.mouse[0].first), BTN_RIGHT);
}

TEST(engine_axes_are_forwarded) {
    test::RecordingSink sink;
    Engine e(&sink);
    e.set_profile(Profile::make_default());

    InputState st;
    st.axis[static_cast<std::size_t>(Axis::X)] = 512;
    st.axis[static_cast<std::size_t>(Axis::Throttle)] = 200;
    e.process(st, InputState{});
    CHECK(sink.axes.size() >= kAxisCount);
    CHECK_EQ(sink.axes[0].first, 0);
    CHECK_EQ(sink.axes[0].second, 512);
    CHECK_EQ(sink.axes[3].second, 200);
}

TEST(engine_axis_deadzone_applies) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.axes[static_cast<std::size_t>(Axis::X)].deadzone = 0.2;
    e.set_profile(p);

    InputState st;
    st.axis[0] = 530;  // ~0.518, inside the deadzone
    e.process(st, InputState{});
    CHECK_EQ(sink.axes[0].second, 512);  // snapped to centre
}

TEST(engine_mfd_controls_cycle_pages) {
    test::RecordingSink sink;
    Engine e(&sink);
    e.set_profile(Profile::make_default());
    CHECK(e.page() == MfdPage::Status);

    InputState empty;
    e.process(with_buttons({Button::MfdSelect}), empty);
    CHECK(e.page() == MfdPage::Custom);
}

TEST(engine_stopwatch) {
    test::RecordingSink sink;
    Engine e(&sink);
    e.set_profile(Profile::make_default());

    e.set_now(1000000);
    e.stopwatch_start(1000000);
    CHECK(e.stopwatch_running());
    CHECK_EQ(e.stopwatch_elapsed_us(2500000), 1500000ull);

    e.stopwatch_stop(2500000);
    CHECK(!e.stopwatch_running());
    CHECK_EQ(e.stopwatch_elapsed_us(9000000), 1500000ull);

    e.stopwatch_reset();
    CHECK_EQ(e.stopwatch_elapsed_us(9000000), 0ull);
}

TEST(engine_stopwatch_format) {
    CHECK_EQ(format_stopwatch(0), std::string("00:00.00"));
    CHECK_EQ(format_stopwatch(61500000), std::string("01:01.50"));
}

TEST(engine_render_status_page) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.name = "TESTPROFILE";
    p.modes[0].name = "NAV";
    e.set_profile(p);

    auto lines = e.render_mfd(0);
    CHECK_EQ(lines[0], std::string("TESTPROFILE"));
    CHECK_EQ(lines[1], std::string("M1 NAV"));
    CHECK_EQ(lines[2], std::string("SHIFT OFF"));
}

TEST(engine_render_custom_page) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.settings.custom_text = {"ALPHA", "BETA", "GAMMA"};
    e.set_profile(p);
    e.set_page(MfdPage::Custom);

    auto lines = e.render_mfd(0);
    CHECK_EQ(lines[0], std::string("ALPHA"));
    CHECK_EQ(lines[1], std::string("BETA"));
    CHECK_EQ(lines[2], std::string("GAMMA"));
}

TEST(engine_render_per_mode_mfd) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[1].has_mfd = true;
    p.modes[1].mfd_lines = {"COMBAT", "WEAPONS HOT", ""};
    e.set_profile(p);
    e.set_mode(2);

    auto lines = e.render_mfd(0);
    CHECK_EQ(lines[0], std::string("COMBAT"));
    CHECK_EQ(lines[1], std::string("WEAPONS HOT"));
    CHECK_EQ(lines[2], std::string("M2 MODE"));
}

TEST(engine_render_is_sanitised) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.settings.custom_text = {"ciao mondo", "àèìòù", "0123456789abcdefgh"};
    e.set_profile(p);
    e.set_page(MfdPage::Custom);

    auto lines = e.render_mfd(0);
    CHECK_EQ(lines[0], std::string("CIAO MONDO"));
    CHECK_EQ(lines[1], std::string("AEIOU"));
    CHECK_EQ(lines[2], std::string("0123456789ABCDEF"));
}

TEST(engine_macro_runs_and_releases) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_default();
    p.modes[0].buttons[1].type = ActionType::Macro;
    p.modes[0].buttons[1].macro = {"ctrl+c"};
    e.set_profile(p);

    e.set_now(1000);
    e.process(with_buttons({Button::Fire}), InputState{});
    // Nothing emitted yet: the macro is scheduled.
    CHECK(sink.keys.empty());

    e.tick(1000);
    CHECK(sink.has_key(KEY_LEFTCTRL, true));
    CHECK(sink.has_key(KEY_C, true));

    e.tick(20000);
    CHECK(sink.has_key(KEY_C, false));
    CHECK(sink.has_key(KEY_LEFTCTRL, false));
}

TEST(macro_parse_rejects_unknown_keys) {
    std::vector<MacroStep> steps;
    std::string err;
    CHECK(!Macro::parse({"ctrl+notakey"}, steps, err));
    CHECK(!err.empty());
    CHECK(Macro::parse({"delay:50", "text:hi", "down:a", "up:a"}, steps, err));
    CHECK_EQ(steps.size(), 4u);
}

TEST(macro_text_types_shifted_characters) {
    std::vector<MacroStep> steps;
    std::string err;
    CHECK(Macro::parse({"text:A"}, steps, err));
    std::vector<std::pair<uint64_t, std::pair<uint16_t, bool>>> events;
    Macro::schedule(steps, 0, events);
    bool shift = false, upper = false;
    for (const auto& ev : events) {
        if (ev.second.first == KEY_LEFTSHIFT) shift = true;
        if (ev.second.first == KEY_A) upper = true;
    }
    CHECK(shift);
    CHECK(upper);
}

TEST(engine_led_theme_reported) {
    test::RecordingSink sink;
    Engine e(&sink);
    Profile p = Profile::make_example();
    e.set_profile(p);

    std::array<LedColor, static_cast<std::size_t>(Led::Count)> colors{};
    CHECK(e.current_leds(colors));
    CHECK(colors[static_cast<std::size_t>(Led::T1)] == LedColor::Green);

    e.set_mode(2);
    CHECK(e.current_leds(colors));
    CHECK(colors[static_cast<std::size_t>(Led::T1)] == LedColor::Red);
}
