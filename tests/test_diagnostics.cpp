// SPDX-License-Identifier: BSD-3-Clause
#include "test_framework.hpp"

#include <cmath>

#include "x52/diagnostics.hpp"
#include "x52/input.hpp"

using namespace x52;

namespace {

InputState stick(int x, int y, int twist) {
    InputState st;
    st.axis[static_cast<std::size_t>(Axis::X)] = x;
    st.axis[static_cast<std::size_t>(Axis::Y)] = y;
    st.axis[static_cast<std::size_t>(Axis::Twist)] = twist;
    return st;
}

}  // namespace

TEST(drift_monitor_requires_start) {
    DriftMonitor m;
    m.add(stick(512, 512, 512));
    CHECK_EQ(m.samples(), 0);
    CHECK(!m.active());
}

TEST(drift_monitor_centred_stick) {
    DriftMonitor m;
    m.start();
    for (int i = 0; i < 100; ++i) m.add(stick(512, 512, 512));
    m.stop();

    CHECK_EQ(m.samples(), 100);
    AxisStats x = m.axis(Axis::X);
    CHECK_NEAR(x.mean, 512.0 / 1023.0, 1e-9);
    CHECK_NEAR(x.offset_from_centre(), 512.0 / 1023.0 - 0.5, 1e-9);
    CHECK_NEAR(x.jitter(), 0.0, 1e-12);
    CHECK(m.worst_centre_offset() < 0.01);
}

TEST(drift_monitor_detects_offset) {
    DriftMonitor m;
    m.start();
    for (int i = 0; i < 50; ++i) m.add(stick(600, 512, 512));
    m.stop();

    AxisStats x = m.axis(Axis::X);
    CHECK(x.offset_from_centre() > 0.05);
    CHECK(m.worst_centre_offset() > 0.05);
}

TEST(drift_monitor_jitter) {
    DriftMonitor m;
    m.start();
    for (int i = 0; i < 10; ++i) m.add(stick(i % 2 ? 500 : 524, 512, 512));
    m.stop();

    AxisStats x = m.axis(Axis::X);
    CHECK(x.jitter() > 0.02);
    CHECK(x.span() > 0.0);
}

TEST(classify_centre_levels) {
    DriftMonitor m;
    m.start();
    for (int i = 0; i < 40; ++i) m.add(stick(512, 512, 512));
    m.stop();
    CHECK(classify_centre(Axis::X, m.axis(Axis::X)).level == CheckLevel::Ok);

    DriftMonitor bad;
    bad.start();
    for (int i = 0; i < 40; ++i) bad.add(stick(700, 512, 512));
    bad.stop();
    CHECK(classify_centre(Axis::X, bad.axis(Axis::X)).level == CheckLevel::Fail);

    AxisStats empty;
    CHECK(classify_centre(Axis::Y, empty).level == CheckLevel::Warn);
}

TEST(check_level_names) {
    CHECK_EQ(std::string(check_level_name(CheckLevel::Ok)), std::string("ok"));
    CHECK_EQ(std::string(check_level_name(CheckLevel::Warn)), std::string("warn"));
    CHECK_EQ(std::string(check_level_name(CheckLevel::Fail)), std::string("fail"));
}
