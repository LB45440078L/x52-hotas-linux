// SPDX-License-Identifier: BSD-3-Clause
#include "test_framework.hpp"

#include <string>

#include "x52/rules.hpp"
#include "x52/telemetry.hpp"

using namespace x52;

namespace {
std::map<std::string, double> sample() {
    return {{"ias", 640.0}, {"alt", 8200.0}, {"mach", 1.15}, {"g", 7.5},
            {"gear", 0.0}, {"fuel_pct", 62.0}};
}
}  // namespace

TEST(condition_simple_comparison) {
    bool r = false;
    std::string err;
    CHECK(evaluate_condition("gear < 0.05", sample(), r, err));
    CHECK(r);
    CHECK(evaluate_condition("alt > 10000", sample(), r, err));
    CHECK(!r);
}

TEST(condition_boolean_logic) {
    bool r = false;
    std::string err;
    CHECK(evaluate_condition("gear < 0.05 and alt > 1000", sample(), r, err));
    CHECK(r);
    CHECK(evaluate_condition("gear < 0.05 and alt > 9000", sample(), r, err));
    CHECK(!r);
    CHECK(evaluate_condition("mach > 2 or g > 7", sample(), r, err));
    CHECK(r);
    CHECK(evaluate_condition("!(gear > 0.5)", sample(), r, err));
    CHECK(r);
}

TEST(condition_arithmetic_and_precedence) {
    bool r = false;
    std::string err;
    CHECK(evaluate_condition("alt / 1000 > 8", sample(), r, err));
    CHECK(r);
    CHECK(evaluate_condition("(g - 7) > 0.4", sample(), r, err));
    CHECK(r);
    CHECK(evaluate_condition("alt - 8200 == 0", sample(), r, err));
    CHECK(r);
}

TEST(condition_missing_value_is_false) {
    bool r = true;
    std::string err;
    CHECK(evaluate_condition("nosuchvalue > 1", sample(), r, err));
    CHECK(!r);
    CHECK(err.empty());
}

TEST(condition_rejects_bad_syntax) {
    bool r = false;
    std::string err;
    CHECK(!evaluate_condition("gear <", sample(), r, err));
    CHECK(!err.empty());
    CHECK(!evaluate_condition("gear < 1 )", sample(), r, err));
    CHECK(!evaluate_condition("", sample(), r, err));
}

TEST(rules_later_wins_and_disabled_skipped) {
    std::vector<TelemetryRule> rules;
    TelemetryRule a;
    a.condition = "gear < 0.05";
    a.led = Led::T3;
    a.color = LedColor::Red;
    rules.push_back(a);

    TelemetryRule b = a;
    b.color = LedColor::Amber;
    rules.push_back(b);

    TelemetryRule off = a;
    off.enabled = false;
    off.led = Led::T1;
    rules.push_back(off);

    auto result = evaluate_rules(rules, sample());
    CHECK(result.count(Led::T3) == 1);
    CHECK(result[Led::T3] == LedColor::Amber);  // later rule won
    CHECK(result.count(Led::T1) == 0);
}

TEST(warthunder_parse_state) {
    const std::string state = R"({
      "valid": true,
      "IAS, km/h": 640, "TAS, km/h": 1105, "H, m": 8200,
      "M": 1.15, "Ny": 7.5, "AoA, deg": 2.1, "Vy, m/s": -12.5,
      "Mfuel, kg": 3100, "Mfuel0, kg": 5000,
      "throttle 1, %": 100, "RPM 1": 9700, "flaps, %": 20
    })";
    const std::string ind = R"({"gears": 0.0, "compass": 271.4, "airbrake_indicator": 0.0})";
    TelemetryValues v;
    std::string err;
    CHECK(parse_warthunder(state, ind, v, err));
    CHECK_EQ(v["ias"], 640.0);
    CHECK_EQ(v["alt"], 8200.0);
    CHECK_NEAR(v["mach"], 1.15, 1e-9);
    CHECK_EQ(v["gear"], 0.0);
    CHECK_NEAR(v["fuel_pct"], 62.0, 1e-6);
    CHECK_NEAR(v["compass"], 271.4, 1e-9);
}

TEST(warthunder_parse_requires_valid) {
    TelemetryValues v;
    std::string err;
    CHECK(!parse_warthunder(R"({"valid": false})", "", v, err));
    CHECK(!err.empty());
}

TEST(template_formatting) {
    auto v = sample();
    CHECK_EQ(format_telemetry_line("{ias:4.0f}KMH", v), std::string(" 640KMH"));
    CHECK_EQ(format_telemetry_line("{alt:5.0f}M", v), std::string(" 8200M"));
    CHECK_EQ(format_telemetry_line("M{mach:.2f}", v), std::string("M1.15"));
    CHECK_EQ(format_telemetry_line("{gear} {fuel_pct:3.0f}", v), std::string("0  62"));
    CHECK_EQ(format_telemetry_line("x{missing}y", v), std::string("x--y"));
}

TEST(telemetry_source_names) {
    TelemetrySourceKind k{};
    CHECK(telemetry_source_from_name("warthunder", k));
    CHECK(k == TelemetrySourceKind::WarThunder);
    CHECK(telemetry_source_from_name("none", k));
    CHECK(k == TelemetrySourceKind::None);
    CHECK(!telemetry_source_from_name("microsoft-flight-simulator", k));
}
