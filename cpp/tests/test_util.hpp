// Shared helpers for the test binaries (value comparison + variable env).
#pragma once

#include "parser/evaluator.hpp"  // kNumVars

#include <array>
#include <cmath>

namespace mp::test {

// NaN matches NaN; exact equality covers ±inf; otherwise relative tolerance.
inline bool nearly(double a, double b) {
    if (std::isnan(b)) return std::isnan(a);
    if (a == b) return true;
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

// Canonical test environment: a=2, b=3, c=4, d=5.
inline std::array<double, kNumVars> testEnv() {
    std::array<double, kNumVars> env{};
    env[0] = 2; env[1] = 3; env[2] = 4; env[3] = 5;
    return env;
}

}  // namespace mp::test
