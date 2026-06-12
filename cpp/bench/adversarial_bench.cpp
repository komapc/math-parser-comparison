// Adversarial (structured, non-random) inputs that separate the multipass
// family asymptotically. Three flat chains, no parentheses:
//
//   powchain   3^2 * 2^2 / 2^2 * 3^3 / 3^3 ...   alternating-precedence (^ vs */)
//   towerchain 1^1^...^1 * 1 * 1 * ...           long ^ run, then a * run
//   sumchain   1 + 2 - 3 + 4 ...                 single-precedence
//
// On powchain the top-down splitters degenerate: the candidate list mixes
// prec 2 and prec 4, so the flat-chain fold never applies and the linear
// right-to-left split scan has no prec-1 early exit — every split rescans its
// whole range: Theta(n^2) (multipass, multipass-arena, direct-mp). The sparse-
// table RMQ (multipass-bfs) answers splits in O(1).
//
// towerchain attacks the OTHER linear scan: the flat-chain *check*. Every
// right-end * split leaves the long same-precedence ^ prefix in the left
// sub-range, and the "is this range flat?" scan rereads it before every split
// — quadratic even when finding the split is O(1), so this one also catches
// multipass-bfs. Bottom-up multipass-reverse reduces each level in one pass
// regardless: Theta(n).
//
// sumchain is the control: one precedence level, the flat-chain fold applies,
// and the whole family is linear — showing the blow-ups are about mixed
// precedence, not chain length.
//
// Usage: ./build/adversarial_bench
#include "parser/evaluator.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <print>
#include <string>
#include <vector>

using namespace mp;
using Clock = std::chrono::steady_clock;

namespace {
volatile double g_sink = 0;

// m factors b^e; ops alternate * and /; adjacent factor pairs are identical,
// so the value stays exactly 9.0 and the cross-check is an equality test.
std::string powChain(int m) {
    std::string s = "3 ^ 2";
    for (int i = 1; i < m; ++i) {
        const int k = (i - 1) / 2;
        s += (i % 2) ? " * " : " / ";
        s += std::to_string(2 + k % 8) + " ^ " + std::to_string(2 + k % 3);
    }
    return s;
}

// m/2 ^ operators in one run, then m-m/2 * factors, all operands 1 — the
// value stays exactly 1.0. m+1 leaves.
std::string towerChain(int m) {
    std::string s = "1";
    for (int i = 0; i < m / 2; ++i) s += " ^ 1";
    for (int i = m / 2; i < m; ++i) s += " * 1";
    return s;
}

// m terms; ops alternate + and -; value stays small.
std::string sumChain(int m) {
    std::string s = "1";
    for (int i = 1; i < m; ++i) {
        s += (i % 2) ? " + " : " - ";
        s += std::to_string(1 + i % 9);
    }
    return s;
}

double bestNs(IEvaluator& ev, const std::string& expr, int reps) {
    double best = std::numeric_limits<double>::infinity();
    for (int r = 0; r < reps; ++r) {
        const auto t0 = Clock::now();
        g_sink = ev.eval(expr);
        best = std::min(best, std::chrono::duration<double, std::nano>(
                                  Clock::now() - t0).count());
    }
    return best;
}

void runShape(const char* title, const std::vector<int>& ms,
              std::string (*gen)(int), int leavesPerFactor) {
    std::println("-- {} --", title);
    std::print("{:<26}", "strategy");
    for (int m : ms) std::print("{:>12}", "m=" + std::to_string(m));
    std::println("   (ns/leaf; flat = linear, ~4x/col = quadratic)");

    std::vector<std::string> exprs;
    for (int m : ms) exprs.push_back(gen(m));

    const auto evs = all_evaluators();
    // cross-check on the largest input
    const double ref = evs.front()->eval(exprs.back());
    for (const auto& ev : evs)
        if (ev->eval(exprs.back()) != ref)
            std::println("MISMATCH [{}]: {} != {}", ev->name(),
                         ev->eval(exprs.back()), ref);

    for (const auto& ev : evs) {
        std::print("{:<26}", ev->name());
        for (std::size_t i = 0; i < ms.size(); ++i) {
            const double ns = bestNs(*ev, exprs[i], 3);
            std::print("{:>12.1f}", ns / (ms[i] * (double)leavesPerFactor));
        }
        std::println("");
    }
    std::println("");
}
}  // namespace

int main() {
    std::println("== C++: adversarial chains (structured inputs, no parens) ==\n");
    runShape("powchain: b^e * b^e / ... (mixed precedence)",
             {512, 2048, 8192}, powChain, 2);
    runShape("towerchain: 1^1^...^1 * 1 * ... (flat-check attack)",
             {512, 2048, 8192}, towerChain, 1);
    runShape("sumchain: 1 + 2 - 3 + ... (single precedence — control)",
             {512, 2048, 8192}, sumChain, 1);
    return 0;
}
