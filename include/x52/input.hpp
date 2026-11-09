// SPDX-License-Identifier: BSD-3-Clause
//
// Reads the joystick's 15-byte HID input report straight from hidraw.  Going
// through hidraw (instead of evdev) keeps the app independent of whatever
// mapping the kernel happens to apply, and gives access to the raw values the
// firmware reports.  evdev is used as a fallback when hidraw is not readable.
//
#ifndef X52_INPUT_HPP
#define X52_INPUT_HPP

#include <array>
#include <cstdint>
#include <string>

#include "x52/protocol.hpp"

namespace x52 {

inline constexpr std::size_t kRawReportSize = 15;

struct InputState {
    std::array<int, kAxisCount> axis{};
    std::array<bool, kButtonCount> buttons{};
    int hat = 0;        // 0 = centred, 1..8 = clockwise starting north
    int thumb_x = 0;    // 0..15
    int thumb_y = 0;    // 0..15
    std::array<uint8_t, kRawReportSize> raw{};  // last report, for diagnostics
    int device_index = -1;

    // Normalised axis value in 0..1 (respecting the raw range of each axis).
    double normalized(Axis a) const;
    bool button(Button b) const { return buttons[static_cast<std::size_t>(b)]; }
    bool operator==(const InputState& o) const;
    bool operator!=(const InputState& o) const { return !(*this == o); }
};

// Parity of the 15-byte HID report; exposed for unit tests.
bool parse_report(const uint8_t* data, std::size_t length, InputState& out) noexcept;

class InputReader {
public:
    InputReader() = default;
    ~InputReader();

    InputReader(const InputReader&) = delete;
    InputReader& operator=(const InputReader&) = delete;

    // Locates and opens the device.  Prefers hidraw, falls back to evdev.
    bool open(std::string& error);
    void close();

    int fd() const noexcept { return fd_; }
    bool is_hidraw() const noexcept { return is_hidraw_; }
    const std::string& path() const noexcept { return path_; }

    // Exclusively captures the evdev node so games do not see the physical
    // device while the daemon is remapping.  Safe to call repeatedly.
    bool set_grab(bool on);
    bool is_grabbed() const noexcept { return grab_fd_ >= 0; }

    // Reads at most one report.  Returns:
    //   1  -> a report was parsed into `state`
    //   0  -> nothing pending
    //  -1  -> device error (probably disconnected)
    int poll(InputState& state);

private:
    int fd_ = -1;
    int grab_fd_ = -1;
    bool is_hidraw_ = false;
    std::string path_;
    InputState evdev_state_{};

    bool open_hidraw(std::string& error);
    bool open_evdev(std::string& error);
    int read_evdev(InputState& state);
};

// Utility used by the daemon and the CLI to find the input device path.
std::string find_hidraw_path();
std::string find_evdev_path();

}  // namespace x52

#endif  // X52_INPUT_HPP
