// SPDX-License-Identifier: GPL-2.0-or-later
#include "x52/input.hpp"

#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace x52 {
namespace {

std::string read_first_line(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[512] = {};
    std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::vector<std::string> glob_paths(const char* pattern) {
    std::vector<std::string> out;
    glob_t g{};
    if (glob(pattern, 0, nullptr, &g) == 0) {
        for (std::size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    }
    globfree(&g);
    return out;
}

}  // namespace

double InputState::normalized(Axis a) const {
    auto i = static_cast<std::size_t>(a);
    if (i >= axis.size()) return 0.0;
    int v = axis[i];
    int max = (a == Axis::X || a == Axis::Y || a == Axis::Twist) ? 1023 : 255;
    if (v < 0) v = 0;
    if (v > max) v = max;
    return static_cast<double>(v) / static_cast<double>(max);
}

bool InputState::operator==(const InputState& o) const {
    return axis == o.axis && buttons == o.buttons && hat == o.hat &&
           thumb_x == o.thumb_x && thumb_y == o.thumb_y;
}

bool parse_report(const uint8_t* d, std::size_t length, InputState& out) noexcept {
    if (!d || length < kRawReportSize) return false;

    uint32_t w0 = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) |
                  (static_cast<uint32_t>(d[2]) << 16) | (static_cast<uint32_t>(d[3]) << 24);

    out.axis[static_cast<std::size_t>(Axis::X)] = static_cast<int>(w0 & 0x3ff);
    out.axis[static_cast<std::size_t>(Axis::Y)] = static_cast<int>((w0 >> 10) & 0x3ff);
    // bits 20..21 are padding
    out.axis[static_cast<std::size_t>(Axis::Twist)] = static_cast<int>((w0 >> 22) & 0x3ff);
    out.axis[static_cast<std::size_t>(Axis::Throttle)] = d[4];
    out.axis[static_cast<std::size_t>(Axis::RotaryX)] = d[5];
    out.axis[static_cast<std::size_t>(Axis::RotaryY)] = d[6];
    out.axis[static_cast<std::size_t>(Axis::Slider)] = d[7];

    uint64_t bits = 0;
    for (int i = 0; i < 5; ++i) {
        bits |= static_cast<uint64_t>(d[8 + i]) << (8 * i);
    }
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        out.buttons[i] = (bits >> i) & 1u;
    }

    out.hat = (d[13] >> 4) & 0x0f;
    out.thumb_y = d[14] & 0x0f;
    out.thumb_x = (d[14] >> 4) & 0x0f;
    return true;
}

std::string find_hidraw_path() {
    for (const auto& sys : glob_paths("/sys/class/hidraw/hidraw*")) {
        std::string uevent = read_first_line(sys + "/device/uevent");
        std::string uevent_all;
        {
            FILE* f = std::fopen((sys + "/device/uevent").c_str(), "r");
            if (f) {
                char buf[1024];
                std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
                uevent_all.assign(buf, n);
                std::fclose(f);
            }
        }
        if (uevent_all.find("000006A3") == std::string::npos) continue;
        if (uevent_all.find("00000762") == std::string::npos) continue;
        std::string name = sys.substr(sys.find_last_of('/') + 1);
        return "/dev/" + name;
    }
    return {};
}

std::string find_evdev_path() {
    for (const auto& sys : glob_paths("/sys/class/input/event*")) {
        std::string name = read_first_line(sys + "/device/name");
        if (name.find("X52") == std::string::npos) continue;
        std::string ev = sys.substr(sys.find_last_of('/') + 1);
        return "/dev/input/" + ev;
    }
    return {};
}

InputReader::~InputReader() { close(); }

void InputReader::close() {
    set_grab(false);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    path_.clear();
    is_hidraw_ = false;
}

bool InputReader::set_grab(bool on) {
    if (on == is_grabbed()) return true;
    if (!on) {
        if (grab_fd_ >= 0) {
            ioctl(grab_fd_, EVIOCGRAB, 0);
            ::close(grab_fd_);
            grab_fd_ = -1;
        }
        return true;
    }
    std::string path = find_evdev_path();
    if (path.empty()) return false;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    if (ioctl(fd, EVIOCGRAB, 1) != 0) {
        ::close(fd);
        return false;
    }
    grab_fd_ = fd;
    return true;
}

bool InputReader::open(std::string& error) {
    close();
    if (open_hidraw(error)) return true;
    std::string hidraw_error = error;
    if (open_evdev(error)) return true;

    if (!hidraw_error.empty()) {
        error = hidraw_error + " / " + error;
    }
    return false;
}

bool InputReader::open_hidraw(std::string& error) {
    std::string path = find_hidraw_path();
    if (path.empty()) {
        error = "no hidraw device for 06a3:0762";
        return false;
    }
    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    fd_ = fd;
    path_ = path;
    is_hidraw_ = true;
    return true;
}

bool InputReader::open_evdev(std::string& error) {
    std::string path = find_evdev_path();
    if (path.empty()) {
        error = "no evdev device for the X52";
        return false;
    }
    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }

    // Seed the state with the current axis positions so the first delta is sane.
    struct input_absinfo info{};
    auto seed_abs = [&](int code, Axis a, int max) {
        if (ioctl(fd, EVIOCGABS(code), &info) == 0) {
            int range = info.maximum - info.minimum;
            int value = range > 0 ? (info.value - info.minimum) * max / range : 0;
            evdev_state_.axis[static_cast<std::size_t>(a)] = value;
        }
    };
    seed_abs(ABS_X, Axis::X, 1023);
    seed_abs(ABS_Y, Axis::Y, 1023);
    seed_abs(ABS_RZ, Axis::Twist, 1023);
    seed_abs(ABS_Z, Axis::Throttle, 255);
    seed_abs(ABS_RX, Axis::RotaryX, 255);
    seed_abs(ABS_RY, Axis::RotaryY, 255);
    seed_abs(ABS_MISC, Axis::Slider, 255);

    fd_ = fd;
    path_ = path;
    is_hidraw_ = false;
    return true;
}

int InputReader::poll(InputState& state) {
    if (fd_ < 0) return -1;
    if (is_hidraw_) {
        uint8_t buf[32];
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            return -1;
        }
        if (n == 0) return 0;
        if (!parse_report(buf, static_cast<std::size_t>(n), state)) return 0;
        return 1;
    }
    return read_evdev(state);
}

int InputReader::read_evdev(InputState& state) {
    struct input_event ev{};
    bool got = false;

    for (;;) {
        ssize_t n = ::read(fd_, &ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return -1;
        }
        if (n != static_cast<ssize_t>(sizeof(ev))) break;

        if (ev.type == EV_ABS) {
            int max = 255;
            int idx = -1;
            switch (ev.code) {
                case ABS_X: idx = static_cast<int>(Axis::X); max = 1023; break;
                case ABS_Y: idx = static_cast<int>(Axis::Y); max = 1023; break;
                case ABS_RZ: idx = static_cast<int>(Axis::Twist); max = 1023; break;
                case ABS_Z: idx = static_cast<int>(Axis::Throttle); break;
                case ABS_RX: idx = static_cast<int>(Axis::RotaryX); break;
                case ABS_RY: idx = static_cast<int>(Axis::RotaryY); break;
                case ABS_MISC: idx = static_cast<int>(Axis::Slider); break;
                case ABS_HAT0X: evdev_state_.thumb_x = ev.value + 7; continue;
                case ABS_HAT0Y: evdev_state_.thumb_y = ev.value + 7; continue;
                default: continue;
            }
            if (idx >= 0) {
                int v = ev.value;
                if (v > max) v = max;
                if (v < 0) v = 0;
                evdev_state_.axis[static_cast<std::size_t>(idx)] = v;
                got = true;
            }
        } else if (ev.type == EV_KEY && ev.code >= BTN_JOYSTICK) {
            int idx = ev.code - BTN_JOYSTICK;
            if (idx >= 0 && static_cast<std::size_t>(idx) < kButtonCount) {
                evdev_state_.buttons[static_cast<std::size_t>(idx)] = ev.value != 0;
                got = true;
            }
        }
    }

    if (got) state = evdev_state_;
    return got ? 1 : 0;
}

}  // namespace x52
