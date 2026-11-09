// SPDX-License-Identifier: BSD-3-Clause
//
// Thin RAII wrapper around libusb for the X52 Pro vendor control channel.
//
#ifndef X52_DEVICE_HPP
#define X52_DEVICE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x52 {

struct DeviceInfo {
    uint16_t bus = 0;
    uint8_t  address = 0;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    std::string manufacturer;
    std::string product;
    std::string serial;
    std::string sysfs_path;
};

enum class DeviceError {
    None = 0,
    NoDevice,
    AccessDenied,
    Io,
    Timeout,
    Disconnected,
    Other,
};

const char* device_error_name(DeviceError e) noexcept;

// Abstract command channel.  The production implementation talks to the
// hardware through libusb; tests substitute a recorder so the exact byte
// stream can be asserted.
class Transport {
public:
    virtual ~Transport() = default;
    virtual DeviceError send(uint16_t command, uint16_t value) = 0;
    virtual bool is_open() const = 0;
};

class Device : public Transport {
public:
    Device();
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;

    // Enumerate matching devices.  Does not require any privileges.
    static std::vector<DeviceInfo> enumerate(uint16_t vid = 0x06a3,
                                             uint16_t pid = 0x0762);

    // True when the process may open the device node for writing.  Used to
    // produce an actionable error instead of a bare "permission denied".
    static bool has_write_access();

    // Opens the first matching device.  On failure `error()` explains why.
    bool open(uint16_t vid = 0x06a3, uint16_t pid = 0x0762);
    void close();
    bool is_open() const noexcept override;

    // Issues one vendor control request.  Retries on transient failures.
    DeviceError send(uint16_t command, uint16_t value) override;
    DeviceError send_checked(uint16_t command, uint16_t value);

    // Delay inserted after every successful transfer.  Newer Logitech
    // firmware silently drops back-to-back MFD appends, so a small gap is
    // needed for the character stream to be accepted.
    void set_write_delay_us(unsigned us) { write_delay_us_ = us; }
    unsigned write_delay_us() const noexcept { return write_delay_us_; }

    DeviceError error() const noexcept { return last_error_; }
    const std::string& error_string() const noexcept { return error_string_; }
    std::string describe() const;

    // Exposed for tests / diagnostics.
    static std::string libusb_error_name(int code);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    DeviceError last_error_ = DeviceError::None;
    std::string error_string_;
    unsigned write_delay_us_ = 2000;
};

// Maps an errno-ish failure to a DeviceError.
DeviceError error_from_errno(int err) noexcept;

}  // namespace x52

#endif  // X52_DEVICE_HPP
