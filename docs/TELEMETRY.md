# Game telemetry

Flight data from a running game can drive the MFD and the LEDs. Nothing hooks
the game, reads its memory or injects input: every source is a supported,
read-only data feed.

## Why not a hook

War Thunder runs BattleEye. Hooking or memory-reading a protected process is
both a ban risk and unnecessary, because the game already publishes everything
on a local HTTP server. The same reasoning applies to the alternatives:
DCS has `export.lua`, Elite Dangerous writes journal files, MSFS has SimConnect.
All of them are plain data, so they fit the same pipeline.

## War Thunder

While a battle or test flight is running the client serves JSON on
`localhost:8111` (it is closed the rest of the time). Two endpoints are used:

| Endpoint | Contents |
| --- | --- |
| `/state` | IAS/TAS, altitude, mach, G, AoA, vertical speed, fuel, gear, flaps, engines |
| `/indicators` | gear animation (0..1), compass, bank, airbrake, vario, weapon flags |

Polled at 5 Hz from a dedicated thread, so a slow game never delays stick
input.

### Value names

`ias tas alt mach g aoa vy fuel fuel_pct gear flaps airbrake throttle rpm
compass bank vario weapon2 weapon3 g_peak valid`

All values are numbers, so a condition never has to deal with strings.

## MFD layout

Each of the three lines is a template. `{name}` prints the value, and
`{name:spec}` uses a printf specifier:

```
{ias:4.0f}KMH {alt:5.0f}M     ->     642KMH  8240M
M{mach:.2f} G{g:.1f}          ->    M1.16 G7.4
{fuel_pct:3.0f}% {gear}       ->      62% 0
```

Missing values render as `--`. Lines are 16 cells wide; longer text is
truncated by the display.

## LED rules

A rule is a condition over telemetry plus a target LED and colour. Rules are
evaluated 5 times per second, top to bottom, and later rules win, so the list
reads like a priority stack. When a rule stops matching, the LED returns to
the profile's mode colour.

```json
"telemetry_rules": [
  { "when": "gear < 0.05",                 "led": "t3", "color": "red" },
  { "when": "gear > 0.05 and gear < 0.95", "led": "t3", "color": "amber" },
  { "when": "alt > 8000",                  "led": "t1", "color": "green" },
  { "when": "g > 7 or g < -3",             "led": "fire", "color": "red" }
]
```

The condition language is arithmetic and boolean logic only - no strings, no
calls, no I/O, so a profile cannot execute anything:

```
+ - * /   < <= > >= == !=   and or not   ( )
```

## Enabling it

Panel: **Telemetry** -> pick the game -> edit the three lines and the rules ->
**Apply layout** -> **Show on MFD**.

Command line:

```bash
x52ctl telemetry                      # status, values, rendered lines
x52ctl telemetry source warthunder
x52ctl json '{"cmd":"set_page","args":{"page":"telemetry"}}'
```

The source, layout and rules are stored in the active profile and autosaved,
so they come back with the profile on the next start.

## Testing without the game

`tools/fake-warthunder.py` serves a realistic `/state` and `/indicators`:

```bash
python3 tools/fake-warthunder.py 8111
x52ctl telemetry source warthunder
x52ctl telemetry
```

## Adding another game

Implement `TelemetrySource` (one `poll()` that fills the value map) and
register it in `make_telemetry_source()`. The template formatter, the rule
engine, the panel and the daemon stay untouched.
