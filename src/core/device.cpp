// SPDX-License-Identifier: GPL-2.0-or-later
#include "x52/device.hpp"

#include <libusb.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "x52/protocol.hpp"

namespace x52 {
namespace {

std::string read_attr(const std::string& dir, const char* name) {
    std::string path = dir + "/" + name;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[256] = {};
    std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string out(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string open_error_hint() {
    return "Cannot open the X52 Pro USB device for writing. "
           "Install the udev rule (see data/udev/99-x52pro.rules) and re-plug the "
           "device, or run with sufficient privileges.";
}

}  // namespace

const char* device_error_name(DeviceError e) noexcept {
    switch (e) {
        case DeviceError::None:         return "none";
        case DeviceError::NoDevice:     return "no-device";
        case DeviceError::AccessDenied: return "access-denied";
        case DeviceError::Io:           return "io-error";
        case DeviceError::Timeout:      return "timeout";
        case DeviceError::Disconnected: return "disconnected";
        case DeviceError::Other:        return "error";
    }
    return "error";
}

DeviceError error_from_errno(int err) noexcept {
    switch (err) {
        case 0:          return DeviceError::None;
        case EACCES:
        case EPERM:      return DeviceError::AccessDenied;
        case ENOENT:
        case ENODEV:     return DeviceError::Disconnected;
        case ETIMEDOUT:  return DeviceError::Timeout;
        default:         return DeviceError::Io;
    }
}

struct Device::Impl {
    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    uint16_t vid = kVendorId;
    uint16_t pid = kProductId;
    uint8_t bnum = 0;
    uint8_t addr = 0;

    ~Impl() { shutdown(); }

    bool init() {
        if (ctx) return true;
        int rc = libusb_init(&ctx);
        return rc == 0;
    }

    void shutdown() {
        if (handle) {
            libusb_close(handle);
            handle = nullptr;
        }
        if (ctx) {
            libusb_exit(ctx);
            ctx = nullptr;
        }
    }

    libusb_device_handle* find_and_open(uint16_t v, uint16_t p) {
        if (!init()) return nullptr;
        libusb_device** list = nullptr;
        ssize_t count = libusb_get_device_list(ctx, &list);
        if (count < 0) return nullptr;

        libusb_device_handle* found = nullptr;
        for (ssize_t i = 0; i < count && !found; ++i) {
            libusb_device* d = list[i];
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(d, &desc) != 0) continue;
            if (desc.idVendor != v || desc.idProduct != p) continue;

            libusb_device_handle* h = nullptr;
            if (libusb_open(d, &h) == 0) {
                bnum = libusb_get_bus_number(d);
                addr = libusb_get_device_address(d);
                found = h;
                break;
            }
            // Remember the device location even if we cannot open it, so the
            // error message can point at the right node.
            bnum = libusb_get_bus_number(d);
            addr = libusb_get_device_address(d);
        }
        libusb_free_device_list(list, 1);
        return found;
    }
};

Device::Device() : impl_(std::make_unique<Impl>()) {}
Device::~Device() = default;
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;

std::vector<DeviceInfo> Device::enumerate(uint16_t vid, uint16_t pid) {
    std::vector<DeviceInfo> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return out;

    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        libusb_exit(ctx);
        return out;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* d = list[i];
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(d, &desc) != 0) continue;
        if (desc.idVendor != vid || desc.idProduct != pid) continue;

        DeviceInfo info;
        info.vendor_id = desc.idVendor;
        info.product_id = desc.idProduct;
        info.bus = libusb_get_bus_number(d);
        info.address = libusb_get_device_address(d);
        char sysfs[64];
        std::snprintf(sysfs, sizeof(sysfs), "/dev/bus/usb/%03u/%03u", info.bus, info.address);
        info.sysfs_path = sysfs;

        libusb_device_handle* h = nullptr;
        if (libusb_open(d, &h) == 0) {
            unsigned char buf[256];
            if (desc.iManufacturer &&
                libusb_get_string_descriptor_ascii(h, desc.iManufacturer, buf, sizeof(buf)) > 0) {
                info.manufacturer.assign(reinterpret_cast<char*>(buf));
            }
            if (desc.iProduct &&
                libusb_get_string_descriptor_ascii(h, desc.iProduct, buf, sizeof(buf)) > 0) {
                info.product.assign(reinterpret_cast<char*>(buf));
            }
            if (desc.iSerialNumber &&
                libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, buf, sizeof(buf)) > 0) {
                info.serial.assign(reinterpret_cast<char*>(buf));
            }
            libusb_close(h);
        }
        out.push_back(std::move(info));
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

bool Device::has_write_access() {
    auto devices = enumerate();
    for (const auto& d : devices) {
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", d.bus, d.address);
        if (::access(path, W_OK) == 0) return true;
    }
    return false;
}

bool Device::open(uint16_t vid, uint16_t pid) {
    close();
    last_error_ = DeviceError::None;
    error_string_.clear();

    impl_->handle = impl_->find_and_open(vid, pid);
    if (!impl_->handle) {
        // Distinguish "absent" from "no permission" for a useful message.
        auto found = enumerate(vid, pid);
        if (found.empty()) {
            last_error_ = DeviceError::NoDevice;
            error_string_ = "No X52 Pro (06a3:0762) found on the USB bus.";
        } else {
            last_error_ = DeviceError::AccessDenied;
            error_string_ = open_error_hint();
        }
        return false;
    }
    return true;
}

void Device::close() {
    if (impl_) impl_->shutdown();
}

bool Device::is_open() const noexcept {
    return impl_ && impl_->handle != nullptr;
}

DeviceError Device::send(uint16_t command, uint16_t value) {
    if (!is_open()) {
        last_error_ = DeviceError::Disconnected;
        error_string_ = "Device is not open.";
        return last_error_;
    }

    int rc = 0;
    for (int attempt = 0; attempt < kControlRetries; ++attempt) {
        rc = libusb_control_transfer(impl_->handle, kControlRequestType, kControlRequest,
                                     value, command, nullptr, 0, kControlTimeoutMs);
        if (rc >= 0) {
            last_error_ = DeviceError::None;
            if (write_delay_us_) ::usleep(write_delay_us_);
            return DeviceError::None;
        }
        if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_ACCESS) break;
        ::usleep(2000u << attempt);
    }

    error_string_ = std::string("control transfer failed: ") + libusb_error_name(rc);
    switch (rc) {
        case LIBUSB_ERROR_NO_DEVICE:
        case LIBUSB_ERROR_PIPE:
            last_error_ = DeviceError::Disconnected;
            close();
            break;
        case LIBUSB_ERROR_ACCESS:
            last_error_ = DeviceError::AccessDenied;
            error_string_ = open_error_hint();
            break;
        case LIBUSB_ERROR_TIMEOUT:
            last_error_ = DeviceError::Timeout;
            break;
        default:
            last_error_ = DeviceError::Io;
            break;
    }
    return last_error_;
}

DeviceError Device::send_checked(uint16_t command, uint16_t value) {
    return send(command, value);
}

std::string Device::libusb_error_name(int code) {
    const char* n = ::libusb_error_name(code);
    return n ? std::string(n) : std::string("LIBUSB_ERROR_") + std::to_string(code);
}

std::string Device::describe() const {
    if (!is_open()) return "X52 Pro (not open)";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "X52 Pro @ %03u:%03u (06a3:0762)", impl_->bnum, impl_->addr);
    return buf;
}

}  // namespace x52
