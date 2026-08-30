// Dependency-free correctness suite. Every case is run against every evaluation
// strategy so they are held to an identical specification.
#include "parser/evaluator.hpp"
#include "parser/reeval.hpp"

#include "test_util.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <print>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

using namespace mp;

namespace {

int g_checks = 0;
int g_failures = 0;

struct Case    { std::string_view expr; double expected; };
struct ErrCase { std::string_view expr; };

using mp::test::nearly;

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
    const auto env = mp::test::testEnv();
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
        // IEEE special values — pow/div corners shared across the languages
        {"0 ^ -1",     std::numeric_limits<double>::infinity()},
        {"1 / (0 * -1)", -std::numeric_limits<double>::infinity()},
        {"(0 - 1e155) ^ 3", -std::numeric_limits<double>::infinity()},
        {"(0 / 0) / 0", std::numeric_limits<double>::quiet_NaN()},
        {"(0 - a) ^ 0.5", std::numeric_limits<double>::quiet_NaN()},
        // literal overflow/underflow is a value (inf / 0), not a syntax error
        {"1e400", std::numeric_limits<double>::infinity()},
        {"1e-400 + a", 2},
    };

    const std::vector<ErrCase> errs = {
        {"a +"}, {"(a + b"}, {"a b"}, {"* a"}, {"a + * b"}, {""},
        // stray ')' and adjacent operand groups (regression: bytecode-vm and
        // the re-eval compilers accepted these), and a digitless number
        {"a)"}, {"(a)(b)"}, {"a(3)"}, {"."},
    };

    auto evs = all_evaluators();
    for (auto& ev : evs) {
        for (const auto& c : cases) checkValue(*ev, c, vars);
        for (const auto& e : errs)  checkError(*ev, e);
    }

    // Parallel variants: same spec, plus malformed inputs long enough to fork
    // (regression: pool*/dfork* used to std::terminate / hang when a subtree
    // parsed on a worker thread threw).
    std::string longOk, longTrail, longParen;
    for (int i = 0; i < 400; ++i) {
        longOk += i ? " + a" : "a";
    }
    longTrail = longOk + " +";
    longParen = longOk; longParen[longParen.size() / 2] = ')';
    const Case longCase{longOk, 800};
    const std::vector<ErrCase> longErrs = {{longTrail}, {longParen}};
    auto pevs = parallel_evaluators();
    for (auto& ev : pevs) {
        for (const auto& c : cases) checkValue(*ev, c, vars);
        for (const auto& e : errs)  checkError(*ev, e);
        checkValue(*ev, longCase, vars);
        for (const auto& e : longErrs) checkError(*ev, e);
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
        for (const auto& e : errs) {
            ++g_checks;
            try {
                (void)comp->compile(e.expr)->eval(vars);
                std::println("FAIL [{:<26}] \"{}\" should have thrown",
                            comp->name(), e.expr);
                ++g_failures;
            } catch (const std::exception&) {
                // expected
            }
        }
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

    std::println("\n{} checks across {} evaluators + {} parallel variants + {} compilers, {} failure(s)",
                g_checks, evs.size(), pevs.size(), compilers.size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
