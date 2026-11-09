# Architecture

Four binaries share one library. Only `x52d` touches the hardware.

```
x52core (static library)
├── protocol      command constants, LED/button/axis tables
├── device        libusb control transfers, enumeration, retry, pacing
├── mfd           three-line framebuffer with dirty tracking
├── leds          logical LEDs -> physical red/green pairs, brightness
├── clock         base clock, two offset clocks, date
├── input         hidraw report parser (evdev fallback), exclusive grab
├── uinput        virtual joystick / keyboard / mouse
├── profile       JSON profile model, validation, load/save
├── engine        mode + shift state machine, action dispatch, macros
└── client        JSON-over-unix-socket client used by the CLI and the GUI
```

## Why a daemon

The hardware is a single-input device whose MFD has to be refreshed
continuously (clock, stopwatch, axis readouts). Doing that from the GUI would
tie the display to the window being open. `x52d` owns the device, so:

- the MFD and LEDs keep working with no GUI running;
- input remapping survives application restarts;
- several clients (panel, CLI, scripts) can drive the device concurrently.

## Daemon loop

Single-threaded, `poll(2)` on the listening socket and every client, with a
16 ms tick. No locks, no races:

1. reconnect the device if it disappeared (2 s backoff);
2. drain input reports, run the engine, sync the virtual joystick;
3. run scheduled macro events;
4. every 500 ms re-render the MFD page and flush only dirty lines;
5. every 5 s re-program the clock (the device does not tick on its own);
6. every 200 ms push state to subscribed clients.

## Engine

`Engine` is pure logic and takes an `OutputSink`:

- `UinputSink` in the daemon, which writes to `/dev/uinput`;
- a recording sink in the tests, which asserts the exact event stream.

Button resolution for a press:

```
mode button (27/28/29)  -> hardware mode change, always
shift (5) / clutch (30) -> shift layer modifier
otherwise               -> layer = mode[shift ? shift_buttons : buttons]
                           passthrough in the shift layer falls back to base
                           dispatch according to action type
```

Hardwired MFD controls (MFD select, function, start/stop, reset) only run when
the profile does not bind that button.

## Modes and LEDs

A mode owns a binding layer, an optional LED theme and optional MFD text. When
the mode changes the daemon applies the theme and refreshes the display. A mode
without an `leds` object leaves the current colours alone.

## IPC

Unix socket at `$XDG_RUNTIME_DIR/x52d.sock`, mode 0600, newline-delimited JSON.

```json
{"id":1,"cmd":"set_mode","args":{"mode":2}}
{"id":1,"ok":true,"result":{"mode":2}}
{"event":"state","data":{...}}
```

Errors use `{"ok":false,"error":"..."}`. Commands: `get_state`, `subscribe`,
`set_mode`, `set_page`, `set_text`, `set_led`, `set_brightness`, `set_shift`,
`stopwatch`, `set_clock`, `clock_now`, `leds_refresh`, `profile`
(`list|get|load|save|create|set|delete`), `raw`, `reconnect`, `shutdown`.

The socket is per-user and guarded by filesystem permissions; there is no
authentication beyond that (same model as most session daemons).

## Failure handling

- **Device unplugged** — every transfer can return `NO_DEVICE`; the daemon
  closes, keeps serving state (`connected: false`) and retries every 2 s.
- **Permission denied** — reported once with an actionable message instead of
  retrying in a loop.
- **Grabbed input** — `EVIOCGRAB` is released automatically when the process
  exits, so a crash cannot wedge the controller.
- **Macro storms** — macros are a time-ordered queue drained on tick; a stuck
  key is released when the queue empties.

## Testing strategy

Unit tests cover the parts where a mistake is invisible in the UI:

- protocol tables and LED wiring (wrong ids → wrong colours),
- MFD sanitising, padding and pair encoding, dirty-line behaviour,
- clock bit packing, including negative and out-of-range offsets,
- profile round trip, clamping, validation and path sanitising,
- engine semantics: passthrough, swallow, keys, shift fallback, modes,
  macros, axis shaping, MFD rendering.

`tests/integration.cpp` (`x52_it`) drives real hardware: it writes text,
sweeps the LEDs, programs the clock, parses input reports and restores a clean
state. It refuses to run without `X52_TEST_HARDWARE=1`.
