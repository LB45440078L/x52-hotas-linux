// SPDX-License-Identifier: BSD-3-Clause
//
// Small expression language for LED rules.
//
// Deliberately not a scripting engine: numbers, named telemetry values,
// arithmetic, comparisons and boolean logic.  No strings, no calls, no I/O,
// so a profile can never execute anything.
//
//   gear < 0.05
//   alt > 1500 and gear == 1
//   (g > 7 or g < -3) and alt < 3000
//
#ifndef X52_RULES_HPP
#define X52_RULES_HPP

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "x52/protocol.hpp"

namespace x52 {

// Parses and evaluates a boolean expression against telemetry values.
// Missing values make a comparison false rather than an error.
bool evaluate_condition(const std::string& expression, const std::map<std::string, double>& values,
                        bool& result, std::string& error);

struct TelemetryRule {
    std::string name;
    std::string condition;
    Led led = Led::Fire;
    LedColor color = LedColor::Amber;
    bool enabled = true;

    bool operator==(const TelemetryRule& o) const {
        return name == o.name && condition == o.condition && led == o.led && color == o.color &&
               enabled == o.enabled;
    }
};

// Result of evaluating a rule list: which LED ends up in which colour.
// Later rules win, so a list reads top to bottom like a priority stack.
std::map<Led, LedColor> evaluate_rules(const std::vector<TelemetryRule>& rules,
                                       const std::map<std::string, double>& values);

}  // namespace x52

#endif  // X52_RULES_HPP
