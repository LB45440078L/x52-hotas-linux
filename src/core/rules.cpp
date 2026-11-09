// SPDX-License-Identifier: BSD-3-Clause
#include "x52/rules.hpp"

#include <cctype>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace x52 {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Parser {
    const std::string& src;
    const std::map<std::string, double>& vars;
    std::size_t pos = 0;
    std::string error;

    explicit Parser(const std::string& s, const std::map<std::string, double>& v)
        : src(s), vars(v) {}

    void skip() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
    }

    bool accept(std::string_view token) {
        skip();
        if (src.compare(pos, token.size(), token) != 0) return false;
        pos += token.size();
        return true;
    }

    bool eof() {
        skip();
        return pos >= src.size();
    }

    double parse_or() {
        double left = parse_and();
        for (;;) {
            skip();
            if (accept("||") || accept("or")) {
                double right = parse_and();
                left = (left != 0.0 || right != 0.0) ? 1.0 : 0.0;
            } else {
                return left;
            }
        }
    }

    double parse_and() {
        double left = parse_not();
        for (;;) {
            skip();
            if (accept("&&") || accept("and")) {
                double right = parse_not();
                left = (left != 0.0 && right != 0.0) ? 1.0 : 0.0;
            } else {
                return left;
            }
        }
    }

    double parse_not() {
        skip();
        if (accept("!") || accept("not")) {
            double v = parse_not();
            return v == 0.0 ? 1.0 : 0.0;
        }
        return parse_comparison();
    }

    double parse_comparison() {
        double left = parse_sum();
        skip();
        struct Op {
            const char* text;
            int kind;
        };
        static const Op ops[] = {{"<=", 0}, {">=", 1}, {"==", 2}, {"!=", 3},
                                 {"<", 4},  {">", 5}};
        for (const auto& op : ops) {
            if (src.compare(pos, std::strlen(op.text), op.text) == 0) {
                pos += std::strlen(op.text);
                double right = parse_sum();
                if (std::isnan(left) || std::isnan(right)) return 0.0;
                bool r = false;
                switch (op.kind) {
                    case 0: r = left <= right; break;
                    case 1: r = left >= right; break;
                    case 2: r = left == right; break;
                    case 3: r = left != right; break;
                    case 4: r = left < right; break;
                    default: r = left > right; break;
                }
                return r ? 1.0 : 0.0;
            }
        }
        return left;
    }

    double parse_sum() {
        double left = parse_term();
        for (;;) {
            skip();
            if (accept("+")) left += parse_term();
            else if (accept("-")) left -= parse_term();
            else return left;
        }
    }

    double parse_term() {
        double left = parse_factor();
        for (;;) {
            skip();
            if (accept("*")) left *= parse_factor();
            else if (accept("/")) {
                double d = parse_factor();
                left = (d == 0.0) ? kNaN : left / d;
            } else return left;
        }
    }

    double parse_factor() {
        skip();
        if (pos >= src.size()) {
            error = "unexpected end of expression";
            return kNaN;
        }
        if (accept("(")) {
            double v = parse_or();
            if (!accept(")")) error = "missing ')'";
            return v;
        }
        if (accept("-")) return -parse_factor();
        if (std::isdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '.') {
            char* end = nullptr;
            double v = std::strtod(src.c_str() + pos, &end);
            pos = static_cast<std::size_t>(end - src.c_str());
            return v;
        }
        if (std::isalpha(static_cast<unsigned char>(src[pos])) || src[pos] == '_') {
            std::size_t start = pos;
            while (pos < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) {
                ++pos;
            }
            std::string name = src.substr(start, pos - start);
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto it = vars.find(name);
            return it == vars.end() ? kNaN : it->second;
        }
        error = std::string("unexpected character '") + src[pos] + "'";
        ++pos;
        return kNaN;
    }
};

}  // namespace

bool evaluate_condition(const std::string& expression, const std::map<std::string, double>& values,
                        bool& result, std::string& error) {
    result = false;
    error.clear();
    if (expression.empty()) {
        error = "empty condition";
        return false;
    }
    Parser p(expression, values);
    double v = p.parse_or();
    if (!p.error.empty()) {
        error = p.error;
        return false;
    }
    if (!p.eof()) {
        error = "trailing characters in condition";
        return false;
    }
    result = std::isfinite(v) && v != 0.0;
    return true;
}

std::map<Led, LedColor> evaluate_rules(const std::vector<TelemetryRule>& rules,
                                       const std::map<std::string, double>& values) {
    std::map<Led, LedColor> out;
    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        bool matched = false;
        std::string error;
        if (!evaluate_condition(rule.condition, values, matched, error)) continue;
        if (matched) out[rule.led] = rule.color;
    }
    return out;
}

}  // namespace x52
