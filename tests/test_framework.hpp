// SPDX-License-Identifier: BSD-3-Clause
#ifndef X52_TEST_FRAMEWORK_HPP
#define X52_TEST_FRAMEWORK_HPP

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

int& failure_count();
void report_failure(const char* file, int line, const std::string& message);

}  // namespace testing

#define TEST(name)                                             \
    static void name();                                        \
    static ::testing::Registrar registrar_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ::testing::report_failure(__FILE__, __LINE__, #cond);            \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        auto va = (a);                                                                       \
        auto vb = (b);                                                                       \
        if (!(va == vb)) {                                                                   \
            std::ostringstream os;                                                           \
            os << #a << " == " << #b << " (" << va << " vs " << vb << ")";                   \
            ::testing::report_failure(__FILE__, __LINE__, os.str());                         \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        double va = (a);                                                                     \
        double vb = (b);                                                                     \
        if (std::fabs(va - vb) > (eps)) {                                                    \
            std::ostringstream os;                                                           \
            os << #a << " ~= " << #b << " (" << va << " vs " << vb << ")";                   \
            ::testing::report_failure(__FILE__, __LINE__, os.str());                         \
        }                                                                                    \
    } while (0)

#endif  // X52_TEST_FRAMEWORK_HPP
