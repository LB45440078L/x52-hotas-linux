// SPDX-License-Identifier: BSD-3-Clause
//
// uinput: creates virtual input devices so that remapped buttons and macros
// reach games and the desktop exactly like a real keyboard / mouse / pad.
//
#ifndef X52_UINPUT_HPP
#define X52_UINPUT_HPP

#include <cstdint>
#include <string>

namespace x52 {

class Uinput {
public:
    Uinput() = default;
    ~Uinput();

    Uinput(const Uinput&) = delete;
    Uinput& operator=(const Uinput&) = delete;

    // All three are independent; create only what is needed.
    bool create_joystick(const std::string& name, bool with_hat = true);
    bool create_keyboard(const std::string& name);
    bool create_mouse(const std::string& name);

    bool has_joystick() const noexcept { return joy_fd_ >= 0; }
    bool has_keyboard() const noexcept { return kbd_fd_ >= 0; }
    bool has_mouse() const noexcept { return mouse_fd_ >= 0; }

    void destroy();

    // Joystick
    void axis(int index, int value);
    void button(int index, bool pressed);
    void hat(int direction);  // 0 = centre, 1..8 clockwise from north
    void hat_secondary(int x, int y);  // throttle ministick as a second hat

    // Keyboard / mouse
    void key(uint16_t code, bool pressed);
    void mouse_button(uint16_t button, bool pressed);
    void wheel(int delta);
    void mouse_move(int dx, int dy);

    void sync_joystick();
    void sync_keyboard();
    void sync_mouse();

    const std::string& error() const noexcept { return error_; }

private:
    int joy_fd_ = -1;
    int kbd_fd_ = -1;
    int mouse_fd_ = -1;
    std::string error_;

    bool open_node(int& fd, const std::string& name, uint16_t bustype, uint16_t product);
    static bool enable_key(int fd, uint16_t code);
    static bool enable_abs(int fd, uint16_t code, int min, int max, int fuzz, int flat);
    static bool enable_rel(int fd, uint16_t code);
    bool finish_node(int fd);
};

// key names ("space", "f1", "leftctrl", "a", "1", ...) -> KEY_* code.
uint16_t keycode_from_name(const std::string& name) noexcept;
const char* keycode_name(uint16_t code) noexcept;

}  // namespace x52

#endif  // X52_UINPUT_HPP
