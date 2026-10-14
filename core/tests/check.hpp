// 极简断言工具：不引入外部依赖，保持核心库可独立构建。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hrtest {

inline int g_failures = 0;
inline int g_checks = 0;

inline void report(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
}

inline bool nearly(double a, double b, double eps = 1e-9) {
    const double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= eps * scale;
}

inline int summary(const char* suite) {
    if (g_failures == 0) {
        std::printf("[PASS] %s  (%d checks)\n", suite, g_checks);
        return 0;
    }
    std::printf("[FAIL] %s  (%d of %d checks failed)\n", suite, g_failures, g_checks);
    return 1;
}

}  // namespace hrtest

#define HR_CHECK(expr) ::hrtest::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define HR_CHECK_NEAR(a, b) \
    ::hrtest::report(::hrtest::nearly((a), (b)), #a " ~= " #b, __FILE__, __LINE__)
