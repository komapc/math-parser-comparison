// Dependency-free correctness suite. Every case is run against every evaluation
// strategy so they are held to an identical specification.
#include "parser/evaluator.hpp"
#include "parser/reeval.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

using namespace mp;

namespace {

int g_checks = 0;
int g_failures = 0;

struct Case    { std::string_view expr; double expected; };
struct ErrCase { std::string_view expr; };

bool nearly(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

void checkValue(IEvaluator& ev, const Case& c) {
    ++g_checks;
    try {
        const double got = ev.eval(c.expr);
        if (!nearly(got, c.expected)) {
            std::printf("FAIL [%-26s] \"%.*s\" = %g, expected %g\n",
                        ev.name(), static_cast<int>(c.expr.size()), c.expr.data(),
                        got, c.expected);
            ++g_failures;
        }
    } catch (const std::exception& e) {
        std::printf("FAIL [%-26s] \"%.*s\" threw: %s\n",
                    ev.name(), static_cast<int>(c.expr.size()), c.expr.data(), e.what());
        ++g_failures;
    }
}

void checkError(IEvaluator& ev, const ErrCase& c) {
    ++g_checks;
    try {
        (void)ev.eval(c.expr);
        std::printf("FAIL [%-26s] \"%.*s\" should have thrown\n",
                    ev.name(), static_cast<int>(c.expr.size()), c.expr.data());
        ++g_failures;
    } catch (const std::exception&) {
        // expected
    }
}

}  // namespace

int main() {
    const std::vector<Case> cases = {
        // precedence
        {"2 + 3 * 4", 14},
        {"1 + 2 * 3 - 4 / 2 ^ 2", 6},
        // parentheses override precedence
        {"(2 + 3) * 4", 20},
        {"2 * (3 + 4) - 5", 9},
        // nested parentheses
        {"((1 + 2) * (3 + 4)) / 7", 3},
        {"-(3 + 4) * 2", -14},
        // left-associativity
        {"10 - 2 - 3", 5},
        {"100 / 10 / 2", 5},
        // right-associative ^ and unary interplay
        {"2 ^ 3 ^ 2", 512},
        {"-2 ^ 2", -4},
        {"2 ^ -3", 0.125},
        // literals
        {"3.5 * 2 + 1.5", 8.5},
        {"2e2 + 1", 201},
        // unary stacking (incl. unary plus identity)
        {"--2", 2},
        {"-2 * 3", -6},
        {"+5 - +2", 3},
    };

    const std::vector<ErrCase> errs = {
        {"2 +"}, {"(1 + 2"}, {"1 2"}, {"* 3"}, {"1 + * 2"}, {""},
    };

    auto evs = all_evaluators();
    for (auto& ev : evs) {
        for (const auto& c : cases) checkValue(*ev, c);
        for (const auto& e : errs)  checkError(*ev, e);
    }

    // Re-eval compilers with variables: a=2, b=3, c=4, d=5.
    std::array<double, kNumVars> env{};
    env['a' - 'a'] = 2; env['b' - 'a'] = 3; env['c' - 'a'] = 4; env['d' - 'a'] = 5;
    const std::vector<Case> varCases = {
        {"a + b * c", 14},
        {"(a + b) * c", 20},
        {"a ^ 2 + b", 7},
        {"-a + b", 1},
        {"d / b - a", 5.0 / 3 - 2},
        {"2 + 3 * 4", 14},  // constant still works through compilers
    };
    auto compilers = all_compilers();
    for (auto& comp : compilers) {
        for (const auto& vc : varCases) {
            ++g_checks;
            try {
                const double got = comp->compile(vc.expr)->eval(env.data());
                if (!nearly(got, vc.expected)) {
                    std::printf("FAIL [%-26s] \"%.*s\" = %g, expected %g\n",
                                comp->name(), static_cast<int>(vc.expr.size()),
                                vc.expr.data(), got, vc.expected);
                    ++g_failures;
                }
            } catch (const std::exception& e) {
                std::printf("FAIL [%-26s] \"%.*s\" threw: %s\n", comp->name(),
                            static_cast<int>(vc.expr.size()), vc.expr.data(), e.what());
                ++g_failures;
            }
        }
    }

    std::printf("\n%d checks across %zu evaluators + %zu compilers, %d failure(s)\n",
                g_checks, evs.size(), compilers.size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
