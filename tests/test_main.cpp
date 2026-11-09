// SPDX-License-Identifier: BSD-3-Clause
#include "test_framework.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace testing {
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
int& failure_count() {
    static int n = 0;
    return n;
}
void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message.c_str());
}
}  // namespace testing

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, skipped = 0;
    for (const auto& tc : testing::registry()) {
        if (filter && std::strstr(tc.name, filter) == nullptr) {
            ++skipped;
            continue;
        }
        int before = testing::failure_count();
        tc.fn();
        ++run;
        if (testing::failure_count() == before) {
            std::printf("ok   %s\n", tc.name);
        } else {
            std::printf("FAIL %s\n", tc.name);
        }
    }
    std::printf("\n%d tests, %d skipped, %d failures\n", run, skipped, testing::failure_count());
    return testing::failure_count() == 0 ? 0 : 1;
}
