# Changelog

## 1.0.0 — 2026-11-09

First release: a complete Linux replacement for the Windows X52 Professional
HOTAS driver and profiler.

### Driver
- Vendor control channel (request 0x91) with retry and the 2 ms pacing that
  newer Logitech firmware needs for MFD character appends.
- Throttle MFD: three 16 character lines, five pages (clock, stopwatch,
  status, custom, per-mode), brightness, shift and blink indicators.
- Hardware clock and date, 12/24 hour format, two offset clocks.
- All 11 LEDs with per-half red/green control, brightness, per-mode themes.
- Raw HID input parsing (hidraw) with evdev fallback, exclusive grab and
  uinput re-emission so games see one controller.
- Three modes, pinky shift layer (optional clutch-as-shift), per-mode and
  per-layer bindings, macros with chords, text and delays, axis shaping.
- Profiles as JSON: create, load, save, delete, import, export.
- Hotplug: reconnects on replug, keeps serving state while offline.

### Diagnostics
- Drift monitor with rest position and jitter per axis.
- One-click centre correction stored in the profile.
- Axis feel presets (soft / linear / hard) and deadzone per axis.
- Health check covering permissions, endpoints, virtual devices, grab,
  rest positions, stuck buttons and clock refresh.

### Persistence and settings
- Settings live in `~/.config/x52pro/settings.json` and are written atomically.
- Profile edits are saved automatically ~1.2 s after the last change, so the
  panel no longer needs an explicit Save for LED colours, bindings, text,
  brightness or axis trims.
- The last profile, MFD page and mode are remembered and restored on start.
  The hardware mode selector still wins as soon as the first input report
  arrives: mode is a physical switch, not software state.
- Settings page: start with the system (XDG autostart entry), start minimised,
  verify the link at startup, autosave toggle, and a link test that times 20
  vendor control transfers.

### Live updates
- The daemon pushes compact input frames (raw axes + button bitmask) at up to
  250 Hz, bounded by the device's own report rate (~60 Hz on the tested unit).
- The panel consumes them on a non-blocking socket through the GLib main loop
  instead of polling: measured 59.7 Hz end to end, 16 ms median latency, where
  the previous build polled the daemon four times per second.

### Applications
- `x52d` daemon with a JSON control socket.
- `x52panel` GTK4/libadwaita panel: sidebar navigation, grouped cards,
  live MFD mirror, input test page, profiles.
- `x52ctl` CLI: every feature, plus `test` and `drift`.
- `x52probe` hardware diagnostics and self test.

### Packaging
- udev rule (70-x52pro.rules) covering the USB node, hidraw, evdev and the
  virtual devices created by the daemon.
- systemd user unit, desktop entry, icon, default profiles, installer.
- 70 unit tests with no hardware required, plus a hardware integration test.
