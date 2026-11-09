// SPDX-License-Identifier: GPL-2.0-or-later
//
// X52 Pro HID/vendor protocol definitions.
//
// The X52 Pro exposes a single HID interface whose report descriptor only
// declares input reports.  Everything that drives the throttle MFD (the
// green LCD), the LEDs, the shift indicator and the on-board clocks is
// implemented with vendor control requests:
//
//     bmRequestType = 0x40 (Host->Device, Vendor, Device)
//     bRequest      = 0x91
//     wValue        = data
//     wIndex        = command
//     wLength       = 0
//
// Because these are control transfers they do not require claiming the HID
// interface, so they can be issued while the kernel's usbhid driver owns the
// device.  This is the same mechanism the Windows driver (SaiK0762.sys) and
// the Saitek vendor API use.
//
#ifndef X52_PROTOCOL_HPP
#define X52_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace x52 {

inline constexpr uint16_t kVendorId  = 0x06a3;
inline constexpr uint16_t kProductId = 0x0762;

inline constexpr uint8_t  kControlRequestType = 0x40;  // OUT | VENDOR | DEVICE
inline constexpr uint8_t  kControlRequest     = 0x91;
inline constexpr unsigned kControlTimeoutMs   = 5000;
inline constexpr int      kControlRetries     = 3;

inline constexpr std::size_t kMfdLineWidth = 16;
inline constexpr std::size_t kMfdLineCount = 3;

// ---------------------------------------------------------------------------
// Commands (wIndex)
// ---------------------------------------------------------------------------
enum Cmd : uint16_t {
    // MFD line write: value holds two characters, little endian.
    CmdMfdLine1     = 0xd1,
    CmdMfdLine2     = 0xd2,
    CmdMfdLine3     = 0xd4,
    CmdMfdClearMask = 0x08,   // OR'ed into the line index to clear it

    CmdMfdBrightness = 0xb1,
    CmdLedBrightness = 0xb2,

    // LED on/off: value = (led_id << 8) | on
    CmdLed = 0xb8,

    // Clock 1 (base clock): value = (h24 << 15) | (hour << 8) | minute
    CmdClock1 = 0xc0,
    // Clocks 2 and 3 are offsets from clock 1 in minutes:
    //   value = (h24 << 15) | (negative << 10) | (offset & 0x3ff)
    CmdClock2Offset = 0xc1,
    CmdClock3Offset = 0xc2,

    CmdDateDayMonth = 0xc4,  // value = (month << 8) | day
    CmdDateYear     = 0xc8,

    // Shift indicator on the MFD
    CmdShiftIndicator = 0xfd,
    // Throttle + POV blink indicator
    CmdBlinkIndicator = 0xb4,
};

inline constexpr uint16_t kIndicatorOn  = 0x51;
inline constexpr uint16_t kIndicatorOff = 0x50;

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------
// The eight bicolour LEDs are physically two LEDs (red + green) that are
// addressed independently; amber is red + green at once.
enum class LedId : uint8_t {
    Fire    = 1,
    ARed    = 2,  AGreen    = 3,
    BRed    = 4,  BGreen    = 5,
    DRed    = 6,  DGreen    = 7,
    ERed    = 8,  EGreen    = 9,
    T1Red   = 10, T1Green   = 11,
    T2Red   = 12, T2Green   = 13,
    T3Red   = 14, T3Green   = 15,
    PovRed  = 16, PovGreen  = 17,
    IRed    = 18, IGreen    = 19,
    Throttle = 20,
};

enum class LedColor : uint8_t { Off = 0, Red = 1, Amber = 2, Green = 3 };

// Logical (user facing) LEDs.
enum class Led : uint8_t {
    Fire = 0,
    A,
    B,
    D,
    E,
    T1,
    T2,
    T3,
    Pov,
    Clutch,
    Throttle,
    Count,
};

struct LedWiring {
    LedId red;
    LedId green;
    bool  bicolour;
};

// Returns the physical wiring of a logical LED.
LedWiring led_wiring(Led led) noexcept;

// Human readable names, stable identifiers used by profiles and the IPC API.
std::string_view led_name(Led led) noexcept;
bool led_from_name(std::string_view name, Led& out) noexcept;
std::string_view led_color_name(LedColor c) noexcept;
bool led_color_from_name(std::string_view name, LedColor& out) noexcept;

// ---------------------------------------------------------------------------
// Buttons and axes (indices as reported by the HID input report / evdev)
// ---------------------------------------------------------------------------
enum class Button : uint8_t {
    TriggerPrimary = 0,
    Fire = 1,
    A = 2,
    B = 3,
    C = 4,
    Shift = 5,
    D = 6,
    E = 7,
    T1Up = 8,
    T1Down = 9,
    T2Up = 10,
    T2Down = 11,
    T3Up = 12,
    T3Down = 13,
    TriggerSecondary = 14,
    MousePrimary = 15,
    MouseWheelDown = 16,
    MouseWheelUp = 17,
    MouseSecondary = 18,
    StickHatUp = 19,
    StickHatRight = 20,
    StickHatDown = 21,
    StickHatLeft = 22,
    ThrottleHatUp = 23,
    ThrottleHatRight = 24,
    ThrottleHatDown = 25,
    ThrottleHatLeft = 26,
    Mode1 = 27,
    Mode2 = 28,
    Mode3 = 29,
    Clutch = 30,
    Function = 31,
    StartStop = 32,
    Reset = 33,
    PageUp = 34,
    PageDown = 35,
    Up = 36,
    Down = 37,
    MfdSelect = 38,
};

inline constexpr std::size_t kButtonCount = 39;

std::string_view button_name(Button b) noexcept;
bool button_from_name(std::string_view name, Button& out) noexcept;

enum class Axis : uint8_t {
    X = 0,           // roll
    Y,               // pitch
    Twist,           // yaw (Rz)
    Throttle,        // Z
    RotaryX,         // clutch rotary (Rx)
    RotaryY,         // E rotary (Ry)
    Slider,
    Count,
};

inline constexpr std::size_t kAxisCount = 7;

std::string_view axis_name(Axis a) noexcept;
bool axis_from_name(std::string_view name, Axis& out) noexcept;

// Number of distinct "logical" buttons used for mode/shift bookkeeping.
inline constexpr std::size_t kMaxMouseButtons = 5;

}  // namespace x52

#endif  // X52_PROTOCOL_HPP
