// SPDX-License-Identifier: BSD-3-Clause
//
// Health checks and drift measurement.
//
// The X52 Pro's potentiometers age: a stick at rest rarely reports exactly
// 512, and the rest position wanders.  DriftMonitor collects the raw reports
// so the panel can show the actual rest value and the driver can trim it out.
//
#ifndef X52_DIAGNOSTICS_HPP
#define X52_DIAGNOSTICS_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "x52/input.hpp"
#include "x52/protocol.hpp"

namespace x52 {

struct AxisStats {
    int samples = 0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double stddev = 0.0;

    double span() const { return samples ? maximum - minimum : 0.0; }
    // How far the rest position is from the ideal centre of a centred axis.
    double offset_from_centre() const { return samples ? mean - 0.5 : 0.0; }
    // Peak-to-peak movement while the control is untouched.
    double jitter() const { return span(); }
};

// Samples the axes and keeps running statistics.  Cheap enough to feed every
// input report.
class DriftMonitor {
public:
    void start();
    void stop();
    void reset();
    bool active() const noexcept { return active_; }
    void add(const InputState& state);

    int samples() const noexcept { return samples_; }
    AxisStats axis(Axis a) const;
    // Largest absolute rest offset of the three centred axes, normalised.
    double worst_centre_offset() const;

private:
    bool active_ = false;
    int samples_ = 0;

    std::array<double, kAxisCount> sum_{};
    std::array<double, kAxisCount> sum_sq_{};
    std::array<double, kAxisCount> min_{};
    std::array<double, kAxisCount> max_{};
};

enum class CheckLevel { Ok, Warn, Fail };

struct Check {
    std::string name;
    CheckLevel level = CheckLevel::Ok;
    std::string detail;

    static Check ok(std::string n, std::string d) { return {std::move(n), CheckLevel::Ok, std::move(d)}; }
    static Check warn(std::string n, std::string d) { return {std::move(n), CheckLevel::Warn, std::move(d)}; }
    static Check fail(std::string n, std::string d) { return {std::move(n), CheckLevel::Fail, std::move(d)}; }
};

const char* check_level_name(CheckLevel level) noexcept;

// Classifies the rest position of a centred axis.
Check classify_centre(Axis axis, const AxisStats& stats) noexcept;

}  // namespace x52

#endif  // X52_DIAGNOSTICS_HPP
