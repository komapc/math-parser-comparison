// Dependency-free correctness suite. Every case is run against every evaluation
// strategy so they are held to an identical specification.
#include "parser/evaluator.hpp"
#include "parser/reeval.hpp"

#include <array>
#include <cmath>
#include <print>
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

void checkValue(IEvaluator& ev, const Case& c, const double* vars = nullptr) {
    ++g_checks;
    try {
        const double got = ev.eval(c.expr, vars);
        if (!nearly(got, c.expected)) {
            std::println("FAIL [{:<26}] \"{}\" = {:g}, expected {:g}",
                        ev.name(), c.expr, got, c.expected);
            ++g_failures;
        }
    } catch (const std::exception& e) {
        std::println("FAIL [{:<26}] \"{}\" threw: {}",
                    ev.name(), c.expr, e.what());
        ++g_failures;
    }
}

void checkError(IEvaluator& ev, const ErrCase& c) {
    ++g_checks;
    try {
        (void)ev.eval(c.expr);
        std::println("FAIL [{:<26}] \"{}\" should have thrown",
                    ev.name(), c.expr);
        ++g_failures;
    } catch (const std::exception&) {
        // expected
    }
}

}  // namespace

int main() {
    // Variable environment: a=2, b=3, c=4, d=5.
    std::array<double, kNumVars> env{};
    env['a'-'a'] = 2; env['b'-'a'] = 3; env['c'-'a'] = 4; env['d'-'a'] = 5;
    const double* vars = env.data();

    // All cases exercise the variable code path; a few numeric literals remain
    // for features that cannot be expressed through a-d (scientific notation,
    // unary stacking on a literal, etc.).
    const std::vector<Case> cases = {
        // precedence: a=2, b=3, c=4  →  a+b*c = 2+12 = 14
        {"a + b * c",            14},
        // parentheses override precedence: (a+b)*c = 5*4 = 20
        {"(a + b) * c",          20},
        // mixed: a*(b+c)-d = 2*7-5 = 9
        {"a * (b + c) - d",       9},
        // nested parens: (a+b)*b/d = 5*3/5 = 3
        {"(a + b) * b / d",       3},
        // unary negation: -(b+c)*a = -7*2 = -14
        {"-(b + c) * a",        -14},
        // left-associativity of -: d-a-b = 5-2-3 = 0
        {"d - a - b",             0},
        // left-associativity of /: d*c/a = 20/2 = 10
        {"d * c / a",            10},
        // right-associative ^: a^a^a = 2^(2^2) = 2^4 = 16
        {"a ^ a ^ a",            16},
        // unary/power interplay: -a^a = -(2^2) = -4
        {"-a ^ a",               -4},
        // negative exponent: a^-b = 2^-3 = 0.125
        {"a ^ -b",            0.125},
        // numeric literal + variable: 3.5*a+1.5 = 7+1.5 = 8.5
        {"3.5 * a + 1.5",       8.5},
        // scientific notation literal: 2e2+a = 200+2 = 202
        {"2e2 + a",             202},
        // unary stacking on a literal
        {"--a",                   2},
        // unary on variable: -a*b = -6
        {"-a * b",               -6},
        // unary plus: +d-+a = 5-2 = 3
        {"+d - +a",               3},
    };

    const std::vector<ErrCase> errs = {
        {"a +"}, {"(a + b"}, {"a b"}, {"* a"}, {"a + * b"}, {""},
    };

    auto evs = all_evaluators();
    for (auto& ev : evs) {
        for (const auto& c : cases) checkValue(*ev, c, vars);
        for (const auto& e : errs)  checkError(*ev, e);
    }

    // Re-eval compilers — reuse the same env defined above.
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
                const double got = comp->compile(vc.expr)->eval(vars);
                if (!nearly(got, vc.expected)) {
                    std::println("FAIL [{:<26}] \"{}\" = {:g}, expected {:g}",
                                comp->name(), vc.expr, got, vc.expected);
                    ++g_failures;
                }
            } catch (const std::exception& e) {
                std::println("FAIL [{:<26}] \"{}\" threw: {}",
                            comp->name(), vc.expr, e.what());
                ++g_failures;
            }
        }
    }

    std::println("\n{} checks across {} evaluators + {} compilers, {} failure(s)",
                g_checks, evs.size(), compilers.size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
