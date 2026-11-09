// SPDX-License-Identifier: BSD-3-Clause
//
// Hardware integration test.  Requires the controller and permission to write
// to the USB node; refuses to run unless X52_TEST_HARDWARE=1 so that a plain
// `ctest` on a build machine stays green.
//
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "x52/clock.hpp"
#include "x52/device.hpp"
#include "x52/input.hpp"
#include "x52/leds.hpp"
#include "x52/mfd.hpp"
#include "x52/protocol.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

}  // namespace

int main() {
    if (!std::getenv("X52_TEST_HARDWARE")) {
        std::printf("skipped: set X52_TEST_HARDWARE=1 to run against the device\n");
        return 0;
    }

    auto devices = x52::Device::enumerate();
    check(!devices.empty(), "device enumerated");
    if (devices.empty()) return 1;
    std::printf("     bus %d address %d\n", devices[0].bus, devices[0].address);
    check(x52::Device::has_write_access(), "usb node is writable");

    x52::Device dev;
    check(dev.open(), "device opened");
    if (!dev.is_open()) {
        std::printf("     %s\n", dev.error_string().c_str());
        return 1;
    }
    dev.set_write_delay_us(2000);

    // --- MFD ---------------------------------------------------------------
    x52::Mfd mfd;
    mfd.set_brightness(200);
    mfd.set_lines("X52 IT", "LINE TWO", "LINE THREE");
    check(mfd.flush(dev) == x52::DeviceError::None, "mfd text written");
    usleep(300000);

    mfd.set_line(2, "0123456789ABCDEF");
    check(mfd.flush(dev) == x52::DeviceError::None, "mfd full width line");
    usleep(200000);

    for (int b : {255, 40, 128, 200}) {
        mfd.set_brightness(static_cast<uint8_t>(b));
        check(mfd.flush(dev) == x52::DeviceError::None, "mfd brightness step");
        usleep(80000);
    }

    // --- LEDs --------------------------------------------------------------
    x52::Leds leds;
    leds.set_brightness(220);
    for (auto color : {x52::LedColor::Red, x52::LedColor::Green, x52::LedColor::Amber}) {
        leds.set_all(color);
        check(leds.flush(dev) == x52::DeviceError::None, "led sweep step");
        usleep(200000);
    }
    leds.set_all(x52::LedColor::Off);
    check(leds.flush(dev) == x52::DeviceError::None, "leds off");

    leds.set_shift(true);
    check(leds.flush(dev) == x52::DeviceError::None, "shift indicator on");
    leds.set_shift(false);
    check(leds.flush(dev) == x52::DeviceError::None, "shift indicator off");

    leds.set_blink(true);
    check(leds.flush(dev) == x52::DeviceError::None, "blink on");
    leds.set_blink(false);
    check(leds.flush(dev) == x52::DeviceError::None, "blink off");

    // --- clock -------------------------------------------------------------
    x52::Clock clock;
    clock.set_from_system(true);
    clock.set_offset(1, 60);
    clock.set_offset(2, -300);
    check(clock.flush(dev) == x52::DeviceError::None, "clock programmed");

    // --- input -------------------------------------------------------------
    x52::InputReader reader;
    std::string error;
    check(reader.open(error), "input device opened");
    if (reader.is_hidraw()) {
        std::printf("     reading %s (hidraw)\n", reader.path().c_str());
    } else {
        std::printf("     reading %s (evdev fallback: %s)\n", reader.path().c_str(), error.c_str());
    }

    bool got_report = false;
    x52::InputState state;
    for (int i = 0; i < 400 && !got_report; ++i) {
        if (reader.poll(state) > 0) got_report = true;
        usleep(5000);
    }
    check(got_report, "input report parsed");
    if (got_report) {
        std::printf("     X=%d Y=%d twist=%d throttle=%d\n", state.axis[0], state.axis[1],
                    state.axis[2], state.axis[3]);
        check(state.axis[0] >= 0 && state.axis[0] <= 1023, "x axis in range");
        check(state.axis[3] >= 0 && state.axis[3] <= 255, "throttle in range");
    }

    // --- restore -----------------------------------------------------------
    mfd.set_lines("X52 PRO", "INTEGRATION OK", "");
    mfd.flush(dev);
    leds.set_all(x52::LedColor::Off);
    leds.flush(dev);

    std::printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
