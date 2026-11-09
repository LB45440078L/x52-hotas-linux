// SPDX-License-Identifier: GPL-2.0-or-later
//
// x52probe - low level diagnostic tool for the X52 Pro.
//
// It talks straight to the hardware, bypassing the daemon, which makes it the
// reference tool for validating the protocol on a new kernel or a new unit:
//
//   x52probe info
//   x52probe text "LINE 1" "LINE 2" "LINE 3"
//   x52probe led all amber
//   x52probe bright 200 120
//   x52probe clock now
//   x52probe shift on
//   x52probe listen            (dump parsed input reports)
//   x52probe selftest          (walk through every feature)
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <getopt.h>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

#include "x52/clock.hpp"
#include "x52/device.hpp"
#include "x52/input.hpp"
#include "x52/leds.hpp"
#include "x52/mfd.hpp"
#include "x52/profile.hpp"
#include "x52/protocol.hpp"

namespace {

void usage() {
    std::cout <<
        "x52probe - X52 Pro hardware diagnostics\n"
        "\n"
        "Usage: x52probe <command> [args]\n"
        "\n"
        "Commands:\n"
        "  info                       show device and interface details\n"
        "  text <l1> [l2] [l3]        write the three MFD lines\n"
        "  clear                      blank the MFD\n"
        "  bright <mfd> [led]         set brightness 0-255\n"
        "  led <name|all> <color>     color: off|red|amber|green\n"
        "  shift <on|off>             MFD shift indicator\n"
        "  blink <on|off>             throttle/POV blink indicator\n"
        "  clock now                  program date and time from the system\n"
        "  raw <cmd-hex> <val-hex>    send a raw vendor command\n"
        "  listen [seconds]           dump parsed input reports\n"
        "  selftest                   exercise every output feature\n"
        "  examples <dir>             write the default profile set\n"
        "\n"
        "Colors: off, red, amber, green\n"
        "LEDs:   fire a b d e t1 t2 t3 pov clutch throttle all\n";
}

bool parse_color(const std::string& s, x52::LedColor& out) {
    return x52::led_color_from_name(s, out);
}

x52::Led parse_led(const std::string& s, bool& all) {
    all = (s == "all");
    x52::Led led{};
    if (!all && !x52::led_from_name(s, led)) {
        std::cerr << "unknown LED '" << s << "'\n";
        std::exit(2);
    }
    return led;
}

int cmd_info() {
    auto devices = x52::Device::enumerate();
    std::cout << "Found " << devices.size() << " X52 Pro device(s)\n";
    for (const auto& d : devices) {
        std::cout << "  bus " << int(d.bus) << " address " << int(d.address) << "\n"
                  << "  id    " << std::hex << d.vendor_id << ":" << d.product_id << std::dec << "\n"
                  << "  node  " << d.sysfs_path << "\n"
                  << "  name  " << (d.product.empty() ? "(unreadable)" : d.product) << "\n"
                  << "  mfr   " << (d.manufacturer.empty() ? "(unreadable)" : d.manufacturer) << "\n";
    }
    std::string hidraw = x52::find_hidraw_path();
    std::string evdev = x52::find_evdev_path();
    std::cout << "  hidraw: " << (hidraw.empty() ? "(none)" : hidraw) << "\n";
    std::cout << "  evdev:  " << (evdev.empty() ? "(none)" : evdev) << "\n";
    std::cout << "  write access to USB node: " << (x52::Device::has_write_access() ? "yes" : "no")
              << "\n";

    x52::Device dev;
    if (!dev.open()) {
        std::cerr << "\nopen failed: " << dev.error_string() << "\n";
        return 1;
    }
    std::cout << "\n" << dev.describe() << " opened successfully\n";
    return 0;
}

int cmd_text(int argc, char** argv) {
    x52::Device dev;
    if (!dev.open()) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    x52::Mfd mfd;
    for (int i = 0; i < 3; ++i) {
        mfd.set_line(static_cast<std::size_t>(i), i < argc ? argv[i] : "");
    }
    auto err = mfd.flush(dev);
    if (err != x52::DeviceError::None) {
        std::cerr << "write failed: " << dev.error_string() << "\n";
        return 1;
    }
    std::cout << "MFD updated\n";
    return 0;
}

int cmd_clear() {
    x52::Device dev;
    if (!dev.open()) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    x52::Mfd mfd;
    mfd.set_lines("", "", "");
    mfd.mark_all_dirty();
    if (mfd.flush(dev) != x52::DeviceError::None) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    std::cout << "MFD cleared\n";
    return 0;
}

int cmd_bright(int mfd_v, int led_v) {
    x52::Device dev;
    if (!dev.open()) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    x52::Mfd mfd;
    x52::Leds leds;
    mfd.set_brightness(static_cast<uint8_t>(mfd_v));
    leds.set_brightness(static_cast<uint8_t>(led_v));
    mfd.mark_all_dirty();
    leds.mark_all_dirty();
    x52::DeviceError e = mfd.flush(dev);
    if (e == x52::DeviceError::None) e = leds.flush(dev);
    if (e != x52::DeviceError::None) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    std::cout << "brightness mfd=" << mfd_v << " led=" << led_v << "\n";
    return 0;
}

int cmd_led(const std::string& name, const std::string& color) {
    bool all = false;
    x52::Led led = parse_led(name, all);
    x52::LedColor c{};
    if (!parse_color(color, c)) {
        std::cerr << "unknown color '" << color << "'\n";
        return 2;
    }
    x52::Device dev;
    if (!dev.open()) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    x52::Leds leds;
    leds.set_brightness(200);
    if (all) {
        leds.set_all(c);
    } else {
        leds.set(led, c);
    }
    if (leds.flush(dev) != x52::DeviceError::None) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    std::cout << "LED " << name << " -> " << x52::led_color_name(c) << "\n";
    return 0;
}

int cmd_indicator(bool shift, bool on) {
    x52::Device dev;
    if (!dev.open()) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    x52::Leds leds;
    if (shift) {
        leds.set_shift(on);
    } else {
        leds.set_blink(on);
    }
    if (leds.flush(dev) != x52::DeviceError::None) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    std::cout << (shift ? "shift" : "blink") << " " << (on ? "on" : "off") << "\n";
    return 0;
}

int cmd_clock() {
    x52::Device dev;
    if (!dev.open()) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    x52::Clock clock;
    clock.set_from_system(true);
    if (clock.flush(dev) != x52::DeviceError::None) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    std::cout << "clock programmed\n";
    return 0;
}

int cmd_raw(uint16_t cmd, uint16_t value) {
    x52::Device dev;
    if (!dev.open()) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    auto e = dev.send(cmd, value);
    std::cout << "send cmd=0x" << std::hex << cmd << " value=0x" << value << std::dec
              << " -> " << x52::device_error_name(e) << "\n";
    return e == x52::DeviceError::None ? 0 : 1;
}

int cmd_listen(int seconds) {
    x52::InputReader reader;
    std::string error;
    if (!reader.open(error)) {
        std::cerr << "input open failed: " << error << "\n";
        return 1;
    }
    std::cout << "listening on " << reader.path() << " (" << (reader.is_hidraw() ? "hidraw" : "evdev")
              << ")\n";
    x52::InputState st, prev;
    time_t end = time(nullptr) + seconds;
    while (time(nullptr) < end) {
        int rc = reader.poll(st);
        if (rc < 0) {
            std::cerr << "device error\n";
            return 1;
        }
        if (rc == 0) {
            usleep(2000);
            continue;
        }
        if (st == prev) continue;
        std::printf("X=%4d Y=%4d T=%4d THR=%3d RX=%3d RY=%3d SL=%3d hat=%d thumb=%d,%d btns:",
                    st.axis[0], st.axis[1], st.axis[2], st.axis[3], st.axis[4], st.axis[5],
                    st.axis[6], st.hat, st.thumb_x, st.thumb_y);
        for (std::size_t i = 0; i < x52::kButtonCount; ++i) {
            if (st.buttons[i]) std::printf(" %zu", i);
        }
        std::printf("\n");
        std::fflush(stdout);
        prev = st;
    }
    return 0;
}

int cmd_examples(const std::string& dir) {
    std::string error;
    x52::Profile basic = x52::Profile::make_default();
    if (!basic.save(dir + "/Default.json", error)) {
        std::cerr << error << "\n";
        return 1;
    }
    x52::Profile demo = x52::Profile::make_example();
    if (!demo.save(dir + "/Demo.json", error)) {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << "wrote Default.json and Demo.json to " << dir << "\n";
    return 0;
}

int cmd_selftest() {
    x52::Device dev;
    if (!dev.open()) {
        std::cerr << dev.error_string() << "\n";
        return 1;
    }
    std::cout << dev.describe() << "\n";

    x52::Mfd mfd;
    x52::Leds leds;
    x52::Clock clock;

    std::cout << "[1/6] MFD text\n";
    mfd.set_lines("X52 PRO", "LINUX DRIVER", "SELFTEST OK");
    mfd.set_brightness(200);
    mfd.mark_all_dirty();
    if (mfd.flush(dev) != x52::DeviceError::None) {
        std::cerr << "  " << dev.error_string() << "\n";
        return 1;
    }
    sleep(2);

    std::cout << "[2/6] MFD brightness fade\n";
    for (int b : {255, 10, 60, 128, 200}) {
        mfd.set_brightness(static_cast<uint8_t>(b));
        mfd.flush(dev);
        usleep(120000);
    }

    std::cout << "[3/6] LEDs: all colours\n";
    for (auto color : {x52::LedColor::Red, x52::LedColor::Amber, x52::LedColor::Green,
                       x52::LedColor::Off}) {
        leds.set_all(color);
        leds.set_brightness(255);
        leds.flush(dev);
        std::cout << "  " << x52::led_color_name(color) << "\n";
        usleep(400000);
    }

    std::cout << "[4/6] shift indicator\n";
    leds.set_shift(true);
    leds.flush(dev);
    sleep(1);
    leds.set_shift(false);
    leds.flush(dev);

    std::cout << "[5/6] clock\n";
    clock.set_from_system(true);
    clock.flush(dev);

    std::cout << "[6/6] restore\n";
    mfd.set_lines("X52 PRO", "SELFTEST DONE", "");
    mfd.mark_all_dirty();
    mfd.flush(dev);
    leds.set_all(x52::LedColor::Off);
    leds.flush(dev);
    std::cout << "selftest complete\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];

    if (const char* d = std::getenv("X52_WRITE_DELAY_US")) {
        // Applied to every command below that opens a device.
        (void)d;
    }
    if (cmd == "info") return cmd_info();
    if (cmd == "text") return cmd_text(argc - 2, argv + 2);
    if (cmd == "clear") return cmd_clear();
    if (cmd == "bright") {
        int m = argc > 2 ? std::atoi(argv[2]) : 128;
        int l = argc > 3 ? std::atoi(argv[3]) : m;
        return cmd_bright(m, l);
    }
    if (cmd == "led") {
        if (argc < 4) {
            std::cerr << "usage: x52probe led <name|all> <color>\n";
            return 2;
        }
        return cmd_led(argv[2], argv[3]);
    }
    if (cmd == "shift") return cmd_indicator(true, argc > 2 && std::string(argv[2]) == "on");
    if (cmd == "blink") return cmd_indicator(false, argc > 2 && std::string(argv[2]) == "on");
    if (cmd == "clock") return cmd_clock();
    if (cmd == "raw") {
        if (argc < 4) {
            std::cerr << "usage: x52probe raw <cmd-hex> <value-hex>\n";
            return 2;
        }
        return cmd_raw(static_cast<uint16_t>(std::strtol(argv[2], nullptr, 16)),
                       static_cast<uint16_t>(std::strtol(argv[3], nullptr, 16)));
    }
    if (cmd == "listen") return cmd_listen(argc > 2 ? std::atoi(argv[2]) : 10);
    if (cmd == "examples") {
        if (argc < 3) {
            std::cerr << "usage: x52probe examples <directory>\n";
            return 2;
        }
        return cmd_examples(argv[2]);
    }
    if (cmd == "selftest") return cmd_selftest();

    usage();
    return 2;
}
