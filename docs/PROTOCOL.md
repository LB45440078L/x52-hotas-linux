# X52 Pro protocol notes

Everything the X52 Pro exposes (display, LEDs, indicators, clock) is controlled
through **vendor control requests**. The HID report descriptor only describes
input, so nothing here goes through `HIDIOCSFEATURE`.

## Transport

```
bmRequestType  0x40   Host -> Device, Vendor, Device recipient
bRequest       0x91
wValue         data (command specific, little endian)
wIndex         command
wLength        0
```

Because these are control transfers on endpoint 0 they do **not** require
claiming the HID interface: they work while `usbhid` owns the device. libusb
only needs write permission on `/dev/bus/usb/BBB/DDD`.

**Pacing.** With Logitech firmware 8.x (`bcdDevice 20.00`) MFD character
appends are dropped if they arrive back-to-back; the display stays blank even
though every transfer returns success. Inserting ~2 ms between transfers makes
them reliable. `Device::set_write_delay_us(2000)` does this for every command.

## MFD text

Three lines, 16 characters each. A line is written by clearing it and then
appending eight little-endian character pairs:

| Line | Clear | Append |
| --- | --- | --- |
| 1 | `0xd9` | `0xd1` |
| 2 | `0xda` | `0xd2` |
| 3 | `0xdc` | `0xd4` |

The clear index is the append index OR'ed with `0x08`. `wValue` is
`char[n] | (char[n+1] << 8)`; appends beyond 16 characters are ignored by the
firmware.

Characters are 7-bit ASCII. `0x20`–`0x5f` render as-is; lowercase is folded to
uppercase because the cell set has no lowercase glyphs. Non-ASCII is folded to
the closest ASCII character (`é` → `E`, `°` → `O`) or to `?`.

## LEDs

`wIndex = 0xb8`, `wValue = (led_id << 8) | on`.

| id | LED | id | LED |
| --- | --- | --- | --- |
| 1 | FIRE | 11 | T1 green |
| 2 | A red | 12 | T2 red |
| 3 | A green | 13 | T2 green |
| 4 | B red | 14 | T3 red |
| 5 | B green | 15 | T3 green |
| 6 | D red | 16 | POV red |
| 7 | D green | 17 | POV green |
| 8 | E red | 18 | clutch (i) red |
| 9 | E green | 19 | clutch (i) green |
| 10 | T1 red | 20 | throttle |

Amber is red + green at the same time. FIRE and the throttle LED are single
colour: any non-zero state means "on".

| Function | wIndex | wValue |
| --- | --- | --- |
| LED brightness | `0xb2` | 0–255 |
| MFD brightness | `0xb1` | 0–255 |
| Shift indicator | `0xfd` | `0x51` on / `0x50` off |
| Blink (throttle + POV) | `0xb4` | `0x51` on / `0x50` off |

## Clock and date

The device has no RTC of its own — the host must keep programming it.

```
0xc0  base clock:   1 5..8   8..7      0
                  +---+------+---------+
                  | M | hour | minute  |
                  +---+------+---------+
```

`M` = 1 for 24 h, 0 for 12 h. The hour is always sent in 24 h form.

```
0xc1  clock 2:      1 5..8 10  9..0
                  +---+---+---+----------+
                  | M | 0 | I |  offset  |
                  +---+---+---+----------+
0xc2  clock 3:    same layout
```

`I` = 1 for a negative offset, `offset` is in minutes (10 bits). Offsets beyond
±1023 minutes are folded by whole days.

```
0xc4  month / day:  month << 8 | day
0xc8  year:         two digit year
```

## Input report

15 bytes, no report ID, LSB first per field:

```
byte 0-3   X (10) | Y (10) | pad (2) | twist Rz (10)
byte 4     throttle Z
byte 5     rotary X (Rx)
byte 6     rotary Y (Ry)
byte 7     slider
byte 8-12  buttons 0..38 (39 bits, one bit of padding)
byte 13    pad (4) | hat (4)     1..8 clockwise from north, 0 = centred
byte 14    thumb Y (4) | thumb X (4)
```

Raw ranges: X/Y/twist 0–1023, throttle/rotaries/slider 0–255.

## Buttons

| # | Function | # | Function |
| --- | --- | --- | --- |
| 0 | trigger primary | 20-22 | stick hat E/S/W |
| 1 | FIRE | 23-26 | throttle hat N/E/S/W |
| 2-4 | A, B, C | 27-29 | mode 1/2/3 |
| 5 | SHIFT (pinky) | 30 | clutch (i) |
| 6-7 | D, E | 31 | function |
| 8-13 | T1/T2/T3 up+down | 32 | start/stop |
| 14 | trigger secondary | 33 | reset |
| 15-18 | mouse button/wheel | 34-35 | page up/down |
| 19-22 | stick hat N/E/S/W | 36-37 | up/down |
|  |  | 38 | MFD select |

## What the Windows driver does differently

The Windows package ships `SaiK0762.sys`, which exposes four kernel interfaces
(Toronto, Sydney, DirectOutput, Clock) plus a "Programming" interface that
downloads whole profiles into the device. On Linux none of that is needed:

- remapping is done in userspace with `uinput`, which is both simpler and more
  capable than the firmware's fixed action list;
- profiles are JSON files instead of `.pr0` blobs;
- the hardware clock and MFD/LED protocol is used directly, so the display and
  lights behave exactly like on Windows.

Byte-compatibility with `.pr0` files is not implemented: the format is a
driver-private structure with no public specification.
