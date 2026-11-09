// SPDX-License-Identifier: BSD-3-Clause
#include "x52/diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace x52 {

void DriftMonitor::start() {
    reset();
    active_ = true;
}

void DriftMonitor::stop() { active_ = false; }

void DriftMonitor::reset() {
    samples_ = 0;
    sum_.fill(0.0);
    sum_sq_.fill(0.0);
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        min_[i] = 1.0;
        max_[i] = 0.0;
    }
}

void DriftMonitor::add(const InputState& state) {
    if (!active_) return;
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        double v = state.normalized(static_cast<Axis>(i));
        sum_[i] += v;
        sum_sq_[i] += v * v;
        min_[i] = std::min(min_[i], v);
        max_[i] = std::max(max_[i], v);
    }
    ++samples_;
}

AxisStats DriftMonitor::axis(Axis a) const {
    AxisStats s;
    std::size_t i = static_cast<std::size_t>(a);
    if (i >= kAxisCount || samples_ == 0) return s;

    s.samples = samples_;
    s.mean = sum_[i] / samples_;
    double variance = std::max(0.0, sum_sq_[i] / samples_ - s.mean * s.mean);
    s.stddev = std::sqrt(variance);
    s.minimum = min_[i];
    s.maximum = max_[i];
    return s;
}

double DriftMonitor::worst_centre_offset() const {
    if (samples_ == 0) return 0.0;
    double worst = 0.0;
    for (Axis a : {Axis::X, Axis::Y, Axis::Twist}) {
        worst = std::max(worst, std::fabs(axis(a).offset_from_centre()));
    }
    return worst;
}

const char* check_level_name(CheckLevel level) noexcept {
    switch (level) {
        case CheckLevel::Ok: return "ok";
        case CheckLevel::Warn: return "warn";
        case CheckLevel::Fail: return "fail";
    }
    return "ok";
}

Check classify_centre(Axis axis, const AxisStats& stats) noexcept {
    char detail[128];
    const char* name = axis_name(axis).data();

    if (stats.samples == 0) {
        return Check::warn(std::string(name) + " axis", "no samples yet");
    }

    double offset = stats.offset_from_centre();
    std::snprintf(detail, sizeof(detail), "rest %+.1f%% from centre, jitter %.1f%%",
                  offset * 100.0, stats.jitter() * 100.0);

    // 3% is roughly the mechanical tolerance of a healthy potentiometer; above
    // 8% the stick visibly pulls to one side in game.
    if (std::fabs(offset) > 0.08) {
        return Check::fail(std::string(name) + " axis", detail);
    }
    if (std::fabs(offset) > 0.03 || stats.jitter() > 0.04) {
        return Check::warn(std::string(name) + " axis", detail);
    }
    return Check::ok(std::string(name) + " axis", detail);
}

}  // namespace x52
