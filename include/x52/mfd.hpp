// SPDX-License-Identifier: BSD-3-Clause
//
// Model of the throttle multi-function display: three 16 character lines,
// brightness and a dirty-line mechanism so that we only send what changed.
//
#ifndef X52_MFD_HPP
#define X52_MFD_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "x52/device.hpp"
#include "x52/protocol.hpp"

namespace x52 {

// Converts arbitrary UTF-8 to something the LCD can show.  The display is a
// fixed 5x7-ish segment cell set; ASCII 0x20..0x5f renders, everything else
// is folded to a close ASCII equivalent or to '?'.
std::string mfd_sanitize(std::string_view utf8, std::size_t width = kMfdLineWidth);
char mfd_fold_codepoint(uint32_t cp) noexcept;

class Mfd {
public:
    Mfd() { clear_local(); }

    // Sets a line, padding with spaces and truncating to 16 columns.
    // Returns true when the visible content changed.
    bool set_line(std::size_t line, std::string_view text);
    bool set_lines(std::string_view l1, std::string_view l2, std::string_view l3);

    const std::array<char, kMfdLineWidth>& line(std::size_t line) const;
    std::string line_string(std::size_t line) const;

    // 0..255
    void set_brightness(uint8_t value);
    uint8_t brightness() const noexcept { return brightness_; }

    void mark_all_dirty();
    void mark_all_clean();

    // Pushes pending line/brightness updates to the device.
    DeviceError flush(Transport& dev);

private:
    void clear_local();
    void encode_pair(std::size_t line, std::size_t index, uint16_t& out) const noexcept;

    std::array<std::array<char, kMfdLineWidth>, kMfdLineCount> lines_{};
    std::array<bool, kMfdLineCount> dirty_{};
    uint8_t brightness_ = 128;
    bool brightness_dirty_ = false;
};

}  // namespace x52

#endif  // X52_MFD_HPP
