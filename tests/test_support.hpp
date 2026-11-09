// SPDX-License-Identifier: BSD-3-Clause
#ifndef X52_TEST_SUPPORT_HPP
#define X52_TEST_SUPPORT_HPP

#include <string>
#include <utility>
#include <vector>

#include "x52/device.hpp"
#include "x52/engine.hpp"

namespace x52 {
namespace test {

// Captures the vendor command stream so protocol encoding can be asserted
// without any hardware attached.
struct RecordingTransport : Transport {
    std::vector<std::pair<uint16_t, uint16_t>> commands;
    bool open_state = true;

    DeviceError send(uint16_t command, uint16_t value) override {
        commands.emplace_back(command, value);
        return DeviceError::None;
    }
    bool is_open() const override { return open_state; }

    bool contains(uint16_t cmd, uint16_t value) const {
        for (const auto& c : commands) {
            if (c.first == cmd && c.second == value) return true;
        }
        return false;
    }
    int count(uint16_t cmd) const {
        int n = 0;
        for (const auto& c : commands) {
            if (c.first == cmd) ++n;
        }
        return n;
    }
    uint16_t value_for(uint16_t cmd, int occurrence = 0) const {
        int seen = 0;
        for (const auto& c : commands) {
            if (c.first != cmd) continue;
            if (seen == occurrence) return c.second;
            ++seen;
        }
        return 0;
    }
    void clear() { commands.clear(); }
};

// Records everything the engine emits.
struct RecordingSink : OutputSink {
    struct KeyEvent {
        uint16_t code;
        bool pressed;
    };
    std::vector<KeyEvent> keys;
    std::vector<std::pair<int, bool>> buttons;
    std::vector<std::pair<uint16_t, bool>> mouse;
    std::vector<int> wheels;
    std::vector<std::pair<int, int>> axes;
    std::vector<int> hats;
    std::vector<std::string> execs;
    int syncs = 0;

    void key(uint16_t code, bool pressed) override { keys.push_back({code, pressed}); }
    void mouse_button(uint16_t b, bool pressed) override { mouse.emplace_back(b, pressed); }
    void wheel(int delta) override { wheels.push_back(delta); }
    void axis(int index, int value) override { axes.emplace_back(index, value); }
    void button(int index, bool pressed) override { buttons.emplace_back(index, pressed); }
    void hat(int direction) override { hats.push_back(direction); }
    void sync() override { ++syncs; }
    void exec(const std::string& cmd) override { execs.push_back(cmd); }

    bool has_key(uint16_t code, bool pressed) const {
        for (const auto& k : keys) {
            if (k.code == code && k.pressed == pressed) return true;
        }
        return false;
    }
    bool has_button(int index, bool pressed) const {
        for (const auto& b : buttons) {
            if (b.first == index && b.second == pressed) return true;
        }
        return false;
    }
    void clear() {
        keys.clear();
        buttons.clear();
        mouse.clear();
        wheels.clear();
        axes.clear();
        hats.clear();
        execs.clear();
        syncs = 0;
    }
};

}  // namespace test
}  // namespace x52

#endif  // X52_TEST_SUPPORT_HPP
