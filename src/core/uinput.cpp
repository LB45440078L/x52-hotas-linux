// SPDX-License-Identifier: BSD-3-Clause
#include "x52/uinput.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include "x52/protocol.hpp"

namespace x52 {
namespace {

struct KeyName {
    const char* name;
    uint16_t code;
};

constexpr KeyName kKeyNames[] = {
    {"esc", KEY_ESC}, {"escape", KEY_ESC},
    {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4}, {"5", KEY_5},
    {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9}, {"0", KEY_0},
    {"minus", KEY_MINUS}, {"equal", KEY_EQUAL}, {"backspace", KEY_BACKSPACE},
    {"tab", KEY_TAB},
    {"q", KEY_Q}, {"w", KEY_W}, {"e", KEY_E}, {"r", KEY_R}, {"t", KEY_T},
    {"y", KEY_Y}, {"u", KEY_U}, {"i", KEY_I}, {"o", KEY_O}, {"p", KEY_P},
    {"leftbrace", KEY_LEFTBRACE}, {"rightbrace", KEY_RIGHTBRACE},
    {"enter", KEY_ENTER}, {"return", KEY_ENTER},
    {"leftctrl", KEY_LEFTCTRL}, {"leftcontrol", KEY_LEFTCTRL},
    {"ctrl", KEY_LEFTCTRL}, {"control", KEY_LEFTCTRL},
    {"a", KEY_A}, {"s", KEY_S}, {"d", KEY_D}, {"f", KEY_F}, {"g", KEY_G},
    {"h", KEY_H}, {"j", KEY_J}, {"k", KEY_K}, {"l", KEY_L},
    {"semicolon", KEY_SEMICOLON}, {"apostrophe", KEY_APOSTROPHE},
    {"grave", KEY_GRAVE},
    {"leftshift", KEY_LEFTSHIFT}, {"shift", KEY_LEFTSHIFT},
    {"backslash", KEY_BACKSLASH},
    {"z", KEY_Z}, {"x", KEY_X}, {"c", KEY_C}, {"v", KEY_V}, {"b", KEY_B},
    {"n", KEY_N}, {"m", KEY_M},
    {"comma", KEY_COMMA}, {"dot", KEY_DOT}, {"period", KEY_DOT},
    {"slash", KEY_SLASH}, {"rightshift", KEY_RIGHTSHIFT},
    {"kpasterisk", KEY_KPASTERISK}, {"leftalt", KEY_LEFTALT}, {"alt", KEY_LEFTALT},
    {"space", KEY_SPACE}, {"capslock", KEY_CAPSLOCK},
    {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4},
    {"f5", KEY_F5}, {"f6", KEY_F6}, {"f7", KEY_F7}, {"f8", KEY_F8},
    {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
    {"numlock", KEY_NUMLOCK}, {"scrolllock", KEY_SCROLLLOCK},
    {"kp7", KEY_KP7}, {"kp8", KEY_KP8}, {"kp9", KEY_KP9}, {"kpminus", KEY_KPMINUS},
    {"kp4", KEY_KP4}, {"kp5", KEY_KP5}, {"kp6", KEY_KP6}, {"kpplus", KEY_KPPLUS},
    {"kp1", KEY_KP1}, {"kp2", KEY_KP2}, {"kp3", KEY_KP3},
    {"kp0", KEY_KP0}, {"kpdot", KEY_KPDOT}, {"kpenter", KEY_KPENTER},
    {"rightctrl", KEY_RIGHTCTRL}, {"rightcontrol", KEY_RIGHTCTRL},
    {"kpdivide", KEY_KPSLASH},
    {"rightalt", KEY_RIGHTALT}, {"altgr", KEY_RIGHTALT},
    {"home", KEY_HOME}, {"up", KEY_UP}, {"pageup", KEY_PAGEUP},
    {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"end", KEY_END},
    {"down", KEY_DOWN}, {"pagedown", KEY_PAGEDOWN}, {"insert", KEY_INSERT},
    {"delete", KEY_DELETE}, {"del", KEY_DELETE},
    {"leftmeta", KEY_LEFTMETA}, {"super", KEY_LEFTMETA}, {"win", KEY_LEFTMETA},
    {"meta", KEY_LEFTMETA}, {"cmd", KEY_LEFTMETA},
    {"rightmeta", KEY_RIGHTMETA},
    {"printhscreen", KEY_SYSRQ}, {"sysrq", KEY_SYSRQ},
    {"pause", KEY_PAUSE}, {"menu", KEY_MENU},
    {"mute", KEY_MUTE}, {"volumeup", KEY_VOLUMEUP}, {"volumedown", KEY_VOLUMEDOWN},
    {"playpause", KEY_PLAYPAUSE}, {"nextsong", KEY_NEXTSONG}, {"previoussong", KEY_PREVIOUSSONG},
    {"calculator", KEY_CALC},
};

struct RelName {
    const char* name;
    uint16_t code;
};

[[maybe_unused]] constexpr std::array<RelName, 0> kRelNames{};

std::string ioc_error(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

uint16_t keycode_from_name(const std::string& name) noexcept {
    for (const auto& k : kKeyNames) {
        if (name == k.name) return k.code;
    }
    // Single character fallback: "A" -> KEY_A (case insensitive).
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        for (const auto& k : kKeyNames) {
            if (k.name[0] == c && k.name[1] == '\0') return k.code;
        }
    }
    return 0;
}

const char* keycode_name(uint16_t code) noexcept {
    for (const auto& k : kKeyNames) {
        if (k.code == code) return k.name;
    }
    return "unknown";
}

Uinput::~Uinput() { destroy(); }

void Uinput::destroy() {
    for (int* fd : {&joy_fd_, &kbd_fd_, &mouse_fd_}) {
        if (*fd >= 0) {
            ioctl(*fd, UI_DEV_DESTROY);
            ::close(*fd);
            *fd = -1;
        }
    }
}

bool Uinput::enable_key(int fd, uint16_t code) {
    return ioctl(fd, UI_SET_KEYBIT, code) == 0;
}

bool Uinput::enable_abs(int fd, uint16_t code, int min, int max, int fuzz, int flat) {
    if (ioctl(fd, UI_SET_ABSBIT, code) != 0) return false;
    struct uinput_abs_setup abs{};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.fuzz = fuzz;
    abs.absinfo.flat = flat;
    return ioctl(fd, UI_ABS_SETUP, &abs) == 0;
}

bool Uinput::enable_rel(int fd, uint16_t code) {
    return ioctl(fd, UI_SET_RELBIT, code) == 0;
}

bool Uinput::open_node(int& fd, const std::string& name, uint16_t bustype, uint16_t product) {
    fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error_ = "cannot open /dev/uinput: " + std::string(std::strerror(errno));
        return false;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) != 0) {
        error_ = ioc_error("UI_SET_EVBIT(EV_SYN)");
        return false;
    }

    struct uinput_setup setup{};
    std::snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s", name.c_str());
    setup.id.bustype = bustype;
    setup.id.vendor = kVendorId;
    setup.id.product = product;
    setup.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &setup) != 0) {
        error_ = ioc_error("UI_DEV_SETUP");
        return false;
    }
    return true;
}

bool Uinput::finish_node(int fd) {
    if (ioctl(fd, UI_DEV_CREATE) != 0) {
        error_ = ioc_error("UI_DEV_CREATE");
        return false;
    }
    return true;
}

bool Uinput::create_joystick(const std::string& name, bool with_hat) {
    if (joy_fd_ >= 0) return true;
    int fd = -1;
    if (!open_node(fd, name, BUS_USB, 0x0762)) return false;

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0 || ioctl(fd, UI_SET_EVBIT, EV_ABS) != 0) {
        error_ = ioc_error("UI_SET_EVBIT");
        ::close(fd);
        return false;
    }

    for (int i = 0; i < static_cast<int>(kButtonCount); ++i) {
        if (!enable_key(fd, static_cast<uint16_t>(BTN_TRIGGER + i))) {
            error_ = ioc_error("UI_SET_KEYBIT");
            ::close(fd);
            return false;
        }
    }

    const bool ok =
        enable_abs(fd, ABS_X, 0, 1023, 4, 16) &&
        enable_abs(fd, ABS_Y, 0, 1023, 4, 16) &&
        enable_abs(fd, ABS_RZ, 0, 1023, 4, 16) &&
        enable_abs(fd, ABS_Z, 0, 255, 1, 4) &&
        enable_abs(fd, ABS_RX, 0, 255, 0, 0) &&
        enable_abs(fd, ABS_RY, 0, 255, 0, 0) &&
        enable_abs(fd, ABS_MISC, 0, 255, 0, 0);
    if (!ok) {
        error_ = ioc_error("UI_ABS_SETUP");
        ::close(fd);
        return false;
    }
    if (with_hat) {
        if (!enable_abs(fd, ABS_HAT0X, -1, 1, 0, 0) ||
            !enable_abs(fd, ABS_HAT0Y, -1, 1, 0, 0)) {
            error_ = ioc_error("UI_ABS_SETUP(hat)");
            ::close(fd);
            return false;
        }
    }

    if (!finish_node(fd)) {
        ::close(fd);
        return false;
    }
    joy_fd_ = fd;
    return true;
}

bool Uinput::create_keyboard(const std::string& name) {
    if (kbd_fd_ >= 0) return true;
    int fd = -1;
    if (!open_node(fd, name, BUS_USB, 0x0762)) return false;

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0) {
        error_ = ioc_error("UI_SET_EVBIT");
        ::close(fd);
        return false;
    }
    for (int code = 1; code < 256; ++code) {
        ioctl(fd, UI_SET_KEYBIT, code);
    }
    // Extended keys used by macros (arrows, F-keys, media keys).
    for (int code = KEY_ESC; code <= KEY_MICMUTE; ++code) {
        ioctl(fd, UI_SET_KEYBIT, code);
    }

    if (!finish_node(fd)) {
        ::close(fd);
        return false;
    }
    kbd_fd_ = fd;
    return true;
}

bool Uinput::create_mouse(const std::string& name) {
    if (mouse_fd_ >= 0) return true;
    int fd = -1;
    if (!open_node(fd, name, BUS_USB, 0x0762)) return false;

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0 || ioctl(fd, UI_SET_EVBIT, EV_REL) != 0) {
        error_ = ioc_error("UI_SET_EVBIT");
        ::close(fd);
        return false;
    }
    enable_key(fd, BTN_LEFT);
    enable_key(fd, BTN_RIGHT);
    enable_key(fd, BTN_MIDDLE);
    enable_key(fd, BTN_SIDE);
    enable_key(fd, BTN_EXTRA);
    enable_rel(fd, REL_X);
    enable_rel(fd, REL_Y);
    enable_rel(fd, REL_WHEEL);
    enable_rel(fd, REL_HWHEEL);

    if (!finish_node(fd)) {
        ::close(fd);
        return false;
    }
    mouse_fd_ = fd;
    return true;
}

namespace {
struct EventWriter {
    int fd;
    bool write_event(uint16_t type, uint16_t code, int32_t value) {
        if (fd < 0) return false;
        struct input_event ev{};
        ev.type = type;
        ev.code = code;
        ev.value = value;
        ssize_t n;
        do {
            n = ::write(fd, &ev, sizeof(ev));
        } while (n < 0 && errno == EINTR);
        return n == static_cast<ssize_t>(sizeof(ev));
    }
    void sync() { write_event(EV_SYN, SYN_REPORT, 0); }
};
}  // namespace

void Uinput::axis(int index, int value) {
    static constexpr uint16_t kAxisCodes[kAxisCount] = {
        ABS_X, ABS_Y, ABS_RZ, ABS_Z, ABS_RX, ABS_RY, ABS_MISC,
    };
    if (index < 0 || index >= static_cast<int>(kAxisCount)) return;
    EventWriter{joy_fd_}.write_event(EV_ABS, kAxisCodes[index], value);
}

void Uinput::button(int index, bool pressed) {
    EventWriter{joy_fd_}.write_event(EV_KEY,
                                     static_cast<uint16_t>(BTN_TRIGGER + index),
                                     pressed ? 1 : 0);
}

void Uinput::hat(int direction) {
    EventWriter w{joy_fd_};
    int x = 0, y = 0;
    switch (direction) {
        case 1: y = -1; break;
        case 2: x = 1; y = -1; break;
        case 3: x = 1; break;
        case 4: x = 1; y = 1; break;
        case 5: y = 1; break;
        case 6: x = -1; y = 1; break;
        case 7: x = -1; break;
        case 8: x = -1; y = -1; break;
        default: break;
    }
    w.write_event(EV_ABS, ABS_HAT0X, x);
    w.write_event(EV_ABS, ABS_HAT0Y, y);
    w.sync();
}

void Uinput::key(uint16_t code, bool pressed) {
    EventWriter{kbd_fd_}.write_event(EV_KEY, code, pressed ? 1 : 0);
}

void Uinput::mouse_button(uint16_t button, bool pressed) {
    EventWriter{mouse_fd_}.write_event(EV_KEY, button, pressed ? 1 : 0);
}

void Uinput::wheel(int delta) {
    EventWriter{mouse_fd_}.write_event(EV_REL, REL_WHEEL, delta);
}

void Uinput::mouse_move(int dx, int dy) {
    EventWriter w{mouse_fd_};
    if (dx) w.write_event(EV_REL, REL_X, dx);
    if (dy) w.write_event(EV_REL, REL_Y, dy);
    w.sync();
}

void Uinput::sync_joystick() { EventWriter{joy_fd_}.sync(); }
void Uinput::sync_keyboard() { EventWriter{kbd_fd_}.sync(); }
void Uinput::sync_mouse() { EventWriter{mouse_fd_}.sync(); }

}  // namespace x52
