// SPDX-License-Identifier: BSD-3-Clause
#include "test_framework.hpp"

#include <string>

#include "test_support.hpp"
#include "x52/mfd.hpp"
#include "x52/protocol.hpp"

using namespace x52;

TEST(mfd_sanitize_uppercases_and_pads) {
    std::string s = mfd_sanitize("hello");
    CHECK_EQ(s.size(), kMfdLineWidth);
    CHECK_EQ(s.substr(0, 5), std::string("HELLO"));
    CHECK_EQ(s[5], ' ');
}

TEST(mfd_sanitize_truncates) {
    std::string s = mfd_sanitize("0123456789abcdefXYZ");
    CHECK_EQ(s.size(), kMfdLineWidth);
    CHECK_EQ(s, std::string("0123456789ABCDEF"));
}

TEST(mfd_sanitize_accents) {
    CHECK_EQ(mfd_sanitize("PERCHÉ", 6), std::string("PERCHE"));
    CHECK_EQ(mfd_sanitize("città", 5), std::string("CITTA"));
    CHECK_EQ(mfd_sanitize("50°", 3), std::string("50O"));
}

TEST(mfd_sanitize_controls) {
    CHECK_EQ(mfd_sanitize(std::string("a\tb"), 3), std::string("A B"));
    CHECK_EQ(mfd_sanitize(std::string("a\x01z"), 3), std::string("A?Z"));
}

TEST(mfd_set_line_reports_changes) {
    Mfd mfd;
    CHECK(mfd.set_line(0, "ABC"));
    CHECK(!mfd.set_line(0, "ABC"));
    CHECK(mfd.set_line(0, "ABD"));
    CHECK_EQ(mfd.line_string(0), std::string("ABD"));
    CHECK(!mfd.set_line(5, "x"));  // out of range
}

TEST(mfd_flush_encodes_pairs) {
    test::RecordingTransport t;
    Mfd mfd;
    mfd.set_line(0, "AB");
    CHECK(mfd.flush(t) == DeviceError::None);

    // Clear first, then eight little-endian character pairs.
    CHECK(t.contains(0xd9, 0x0000));
    CHECK_EQ(t.count(0xd1), 8);
    CHECK_EQ(t.value_for(0xd1, 0), 0x4241);  // 'A' | 'B' << 8
    CHECK_EQ(t.value_for(0xd1, 7), 0x2020);  // spaces
}

TEST(mfd_flush_only_dirty_lines) {
    test::RecordingTransport t;
    Mfd mfd;
    mfd.mark_all_clean();  // start from a synced device
    mfd.set_line(1, "HI");
    mfd.flush(t);
    CHECK_EQ(t.count(0xda), 1);  // line 2 clear
    CHECK_EQ(t.count(0xd9), 0);
    CHECK_EQ(t.count(0xd1), 0);

    t.clear();
    mfd.flush(t);
    CHECK_EQ(t.commands.size(), 0u);  // nothing changed
}

TEST(mfd_brightness_emitted_once) {
    test::RecordingTransport t;
    Mfd mfd;
    mfd.set_brightness(200);
    mfd.flush(t);
    CHECK(t.contains(0xb1, 200));
    t.clear();
    mfd.flush(t);
    CHECK_EQ(t.count(0xb1), 0);
}

TEST(mfd_flush_without_device_fails) {
    test::RecordingTransport t;
    t.open_state = false;
    Mfd mfd;
    mfd.set_line(0, "X");
    CHECK(mfd.flush(t) == DeviceError::Disconnected);
}

TEST(mfd_samples_are_sixteen_wide) {
    Mfd mfd;
    mfd.set_line(2, "0123456789ABCDEFGHIJ");
    CHECK_EQ(mfd.line_string(2), std::string("0123456789ABCDEF"));
}
