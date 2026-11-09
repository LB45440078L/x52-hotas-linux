# X52 Pro on Linux

A native, open source driver and control panel for the **Logitech / Saitek X52
Pro H.O.T.A.S.** — the full feature set of the Windows "X52 Professional
HOTAS" software, reimplemented for Linux.

The original Windows package (`X52_Professional_HOTAS_x64_8_0_213_0.exe`)
installs a kernel driver (`SaiK0762.sys`) plus a .NET profiler. This project
replaces both with a small C++ daemon and a GTK4 control panel, driving the
hardware through the documented vendor control channel.

```
    ┌────────────┐   JSON/unix socket   ┌────────────┐    vendor control    ┌──────────────┐
    │  x52panel  │◀────────────────────▶│    x52d    │◀────────────────────▶│  X52 Pro     │
    │  (GTK4)    │                      │  (daemon)  │   0x91 requests      │  throttle +  │
    └────────────┘                      └────────────┘                      │  stick       │
           ▲                                  │  uinput                      └──────────────┘
           │                                  ▼
      x52ctl (CLI)                 virtual joystick / keyboard / mouse
```

## What works

**Throttle multi-function display (the green LCD)**

- Three 16 character lines, live updates, ASCII character folding
- Five pages: clock, stopwatch, live status, custom text, profile info
- Per-mode text overrides
- Display brightness, shift indicator, throttle/POV blink indicator
- Hardware clock: date, 24/12 hour format, two offset clocks (time zones)

**LEDs**

- All eleven LEDs, per-half control (off / red / amber / green)
- Global LED brightness
- Per-mode LED themes that follow the mode selector
- Shift and blink indicators

**Test & diagnostics**

- Live input map at the device report rate (~60 Hz), not a fixed poll
- Stick crosshair and throttle bar visualisation
- Drift measurement: rest position and jitter per axis, with one-click
  centre correction written into the profile
- Axis feel: soft / linear / hard response plus deadzone, per axis
- Health check: permissions, endpoints, virtual devices, grab, rest
  positions, stuck buttons, clock refresh

**Modes, profiles, remapping**

- Three hardware modes (M1/M2/M3) with independent bindings and LED themes
- Pinky shift layer (and optional clutch-as-shift) with per-mode bindings
- Any button can be: passthrough, swallowed, keyboard key, mouse button,
  mouse wheel, macro (chords, typed text, delays), mode switch, MFD page
  switch, shell command, or another joystick button
- Per-axis deadzone, saturation, curve and inversion
- Profiles as readable JSON: create, load, save, delete, import, export
- Automatic hotplug: reconnects when the controller is replugged

**No double input**

The daemon grabs the physical evdev node and re-emits everything through a
virtual joystick, so games see exactly one controller — the remapped one.

## Requirements

- Linux with `evdev`, `uinput` and `hidraw` (any modern distribution)
- libusb 1.0
- GTK4 + libadwaita (only for the panel)
- A C++20 compiler and CMake ≥ 3.20

Tested on CachyOS/Arch with kernel 7.3, GNOME/Wayland, firmware
`bcdDevice 20.00` (Logitech, 2018).

## Install

```bash
git clone <this repo> x52pro-linux
cd x52pro-linux
./scripts/install.sh
```

The script builds everything, installs into `~/.local`, copies the default
profiles, enables the `x52d` user service and tells you if the udev rule is
still missing. The rule itself needs root once:

```bash
sudo ./scripts/install-udev.sh
# then unplug and replug the controller
```

Manual build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/x52_tests        # unit tests, no hardware needed
sudo ./scripts/install-udev.sh
./build/x52panel
```

## Using it

### Control panel

`x52panel` has seven pages:

| Page | What it does |
| --- | --- |
| Overview | device state, live MFD mirror, mode buttons, page selector, axis bars |
| Display | custom text, brightness, clock and date, stopwatch |
| LEDs | per-LED colours, brightness, shift indicator |
| Buttons | per-mode and shift-layer bindings for all 39 buttons |
| Test | live input, stick/throttle visualisation, drift, axis feel, diagnosis |
| Profiles | activate, save, create, delete profiles |
| Settings | autostart, start minimised, autosave, USB link test, capabilities |

### Command line

```bash
x52ctl status                       # device, mode, MFD, axes, LEDs
x52ctl text "HELLO" "FROM LINUX" "" # write the display
x52ctl mode 2                       # switch mode (also via the hardware switch)
x52ctl led t1 amber                 # one LED
x52ctl led all green                # everything
x52ctl bright 200 160               # display / LED brightness
x52ctl page clock                   # MFD page
x52ctl stopwatch start
x52ctl clock --offset2 60 --12h     # base clock + second time zone
x52ctl profile list
x52ctl profile load Demo
x52ctl profile save "Elite Dangerous"
x52ctl profile export elite.json
x52ctl test                         # health check
x52ctl drift 5                      # measure rest positions for 5 s
x52ctl watch                        # live state stream
```

### Raw hardware diagnostics

`x52probe` bypasses the daemon and talks to the device directly — useful to
check a new kernel or a new unit:

```bash
x52probe info
x52probe text "LINE 1" "LINE 2" "LINE 3"
x52probe led all amber
x52probe clock now
x52probe listen 10                  # dump parsed input reports
x52probe selftest                   # exercise every output feature
x52probe raw d1 4241                # send a raw vendor command
```

## Profiles

Profiles live in `~/.config/x52pro/profiles/*.json`. A minimal one:

```json
{
  "schema": 1,
  "name": "Elite Dangerous",
  "settings": {
    "mfd_page": "status",
    "led_brightness": 160,
    "mfd_brightness": 180,
    "grab_device": true
  },
  "modes": {
    "1": {
      "name": "Supercruise",
      "leds": { "fire": "amber", "a": "green", "t1": "green" },
      "mfd": ["SUPERCRUISE", "MODE 1", ""],
      "buttons": {
        "1": { "action": "key", "key": "space" },
        "2": { "action": "macro", "sequence": ["text:hello", "enter"] },
        "4": { "action": "mode", "mode": 2 },
        "6": { "action": "exec", "command": "notify-send x52 hello" }
      }
    }
  }
}
```

Unbound buttons of a shift layer fall back to the base layer, so a shift layer
only needs the entries that actually differ.

Macro steps:

| Step | Meaning |
| --- | --- |
| `ctrl+shift+t` | press the chord, then release it |
| `key:space`, `down:space`, `up:space` | tap / hold / release |
| `text:hello world` | type text, automatically handling shift |
| `delay:250` | wait 250 ms |

## MFD pages

- **Clock** — label, time and date (the device also keeps its own clock)
- **Stopwatch** — host-driven, controlled by START/STOP and RESET, or by CLI
- **Status** — mode, mode name, shift state (or the mode's own text override)
- **Custom** — your three lines
- **Profile** — profile name, mode, description

Hardware buttons: MFD SELECT cycles pages, FUNCTION toggles clock/stopwatch,
START/STOP and RESET drive the stopwatch — unless a profile binds them.

## Troubleshooting

**"Cannot open the X52 Pro USB device for writing"** — the udev rule is not
installed or the device was not replugged. Run `sudo ./scripts/install-udev.sh`
and replug.

**The LCD is lit but the text stays blank** — this is the firmware pacing
quirk: newer Logitech firmware silently drops MFD character appends that
arrive back-to-back. The driver inserts a 2 ms gap between transfers. If you
write your own tool, do the same.

**Two joysticks show up in games** — the daemon should have grabbed the
physical node (`x52ctl status` shows `Grabbed: yes`). If grabbing fails (for
example because another process holds it), disable remapping for that profile
with `"grab_device": false` and use passthrough only.

**The panel says "daemon offline"** — `systemctl --user status x52d`, or start
it by hand: `x52d` (it prints the socket path).

**Mode LEDs do not change** — a mode only drives LEDs if its profile entry has
an `leds` object; otherwise the current colours are left alone.

## Repository layout

```
include/x52/      public headers (protocol, device, mfd, leds, clock, input,
                  uinput, profile, engine, client)
src/core/         library implementation
src/daemon/       x52d
src/cli/          x52ctl
src/gui/          x52panel (GTK4/libadwaita)
src/probe/        x52probe diagnostics
tests/            unit tests (no hardware) + integration test
data/             udev rule, systemd unit, desktop entry, icon, profiles
docs/             protocol and architecture notes
scripts/          install helpers
```

## Development

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/x52_tests                 # 70 unit tests
./build/x52_tests mfd             # filter by name
X52_TEST_HARDWARE=1 ./build/x52_it # integration test, needs the device
```

The protocol is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md), the code
layout in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Credits and prior art

- The vendor API (command indices, LED numbering, clock bit layouts) was
  documented by Saitek and is mirrored in the `libx52` project by Nirenjan
  Krishnan, which is the reference implementation for Linux. The control
  channel details here were verified against a live X52 Pro.
- The Windows driver package was studied for behavioural parity (modes,
  profiles, MFD pages); no code from it is used.
- `libx52` (GPL-2.0) was used only as documentation of the vendor API, which
  Saitek published; no code from it is included here. The implementation in
  this repository was written from scratch against the byte-level protocol and
  verified on a live unit.
- `nlohmann/json` (MIT) is vendored under `third_party/`.

## License

BSD-3-Clause. See [LICENSE](LICENSE).
