// A/B benchmark: single-expression fork-join, async-per-fork (par*) vs the
// persistent-pool / atomic-free rebuild (pool*).
//
// Same generator and methodology as single_par_bench.cpp. For each size we time
// ast-arena (sequential baseline), par1 (sequential parallel path), the shipped
// par2/4/8 (std::async), and the new pool2/4/8. Speedups are reported vs par1
// (isolates thread scaling) and vs ast-arena (the real "did parallelism help a
// single parse at all" question).
#include "parser/evaluator.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <print>
#include <random>
#include <string>
#include <vector>

using namespace mp;
using Clock = std::chrono::steady_clock;

namespace {

volatile std::uint64_t g_sink = 0;

class Generator {
public:
    explicit Generator(std::uint32_t seed) : rng_(seed) {}
    std::string make(int leaves) {
        std::string out;
        out.reserve(static_cast<std::size_t>(leaves) * 5);
        gen(leaves, out);
        return out;
    }
private:
    std::mt19937 rng_;
    int leaf() { return std::uniform_int_distribution<int>(1, 20)(rng_); }
    char op() {
        static const char ops[] = {'+', '+', '-', '-', '*', '/'};
        return ops[std::uniform_int_distribution<int>(0, 5)(rng_)];
    }
    void leafToken(std::string& out) {
        if (std::uniform_int_distribution<int>(0, 4)(rng_) == 0) out += '-';
        out += std::to_string(leaf());
    }
    void gen(int n, std::string& out) {
        if (n <= 1) { leafToken(out); return; }
        const bool paren = std::uniform_int_distribution<int>(0, 2)(rng_) == 0;
        if (paren) out += '(';
        if (std::uniform_int_distribution<int>(0, 6)(rng_) == 0) {
            gen(n - 1, out);
            out += " ^ ";
            out += std::to_string(std::uniform_int_distribution<int>(1, 3)(rng_));
        } else {
            const int l = std::uniform_int_distribution<int>(1, n - 1)(rng_);
            gen(l, out);
            out += ' '; out += op(); out += ' ';
            gen(n - l, out);
        }
        if (paren) out += ')';
    }
};

bool sameResult(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == b) return true;
    const double d = std::fabs(a - b);
    return d <= 1e-6 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

double bestNs(int reps, auto f) {
    double best = std::numeric_limits<double>::infinity();
    for (int r = 0; r < reps; ++r) {
        const auto t0 = Clock::now();
        f();
        const double ns = std::chrono::duration<double, std::nano>(
            Clock::now() - t0).count();
        best = std::min(best, ns);
    }
    return best;
}

}  // namespace

int main() {
    const std::vector<int> sizes = {1000, 10000, 100000, 500000};
    const std::vector<int> repsV = {  30,    15,     10,      5};
    const std::vector<int> cntV  = {   8,     4,      2,      2};

    auto arena  = make_ast_arena();
    auto par1   = make_multipass_par1();
    auto par8   = make_multipass_par8();
    auto pool2  = make_multipass_pool2();
    auto pool4  = make_multipass_pool4();
    auto pool8  = make_multipass_pool8();
    auto dfork2 = make_multipass_dfork2();
    auto dfork4 = make_multipass_dfork4();
    auto dfork8 = make_multipass_dfork8();

    std::println("== pool vs async fork-join: single-expression A/B ==\n");

    // ---- correctness preflight --------------------------------------------
    // Same value at the bit level, but treat every NaN as equal to every other:
    // FP re-association (middle-split vs rightmost) can flip a NaN's sign bit,
    // which is semantically irrelevant.
    auto bitEq = [](double a, double b) {
        if (std::isnan(a) && std::isnan(b)) return true;
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    };
    std::println("Correctness preflight (pool*/dfork* vs arena within 1e-6, vs par1 bit-identical):");
    int errors = 0;
    Generator g(0xBEEF);
    for (int n : {10, 100, 1000, 5000}) {
        for (int i = 0; i < 150; ++i) {
            const std::string e = g.make(n);
            const double ref = arena->eval(e);
            const double v1  = par1->eval(e);
            const double p8  = pool8->eval(e);
            const double d2  = dfork2->eval(e);
            const double d4  = dfork4->eval(e);
            const double d8  = dfork8->eval(e);
            if (!sameResult(ref, p8) || !bitEq(p8, v1) ||
                !bitEq(d2, v1) || !bitEq(d4, v1) || !bitEq(d8, v1)) {
                if (++errors <= 3)
                    std::println("  mismatch: \"{}...\" arena={:g} par1={:g} pool8={:g} dfork8={:g}",
                                 e.substr(0, 40), ref, v1, p8, d8);
            }
        }
    }
    std::println("  {} expressions, {} errors\n", 4 * 150, errors);

    // ---- timing ------------------------------------------------------------
    std::println("{:<16} {:>8} {:>11} {:>9} {:>9}",
                 "strategy", "leaves", "ns/leaf", "x par1", "x arena");
    std::println("{}", std::string(56, '-'));

    for (std::size_t si = 0; si < sizes.size(); ++si) {
        const int n = sizes[si], kReps = repsV[si], kCnt = cntV[si];
        std::vector<std::string> corpus;
        Generator gc(0xC0DE + static_cast<std::uint32_t>(n));
        for (int i = 0; i < kCnt; ++i) corpus.push_back(gc.make(n));

        auto time = [&](IEvaluator& ev) {
            return bestNs(kReps, [&] {
                double acc = 0;
                for (const auto& e : corpus) acc += ev.eval(e);
                g_sink += static_cast<std::uint64_t>(acc);
            }) / static_cast<double>(corpus.size());
        };

        const double a  = time(*arena);
        const double p1 = time(*par1);
        struct Row { const char* label; double ns; };
        const Row rows[] = {
            {"ast-arena", a}, {"par1", p1},
            {"par8",   time(*par8)},
            {"pool2",  time(*pool2)},  {"pool4",  time(*pool4)},  {"pool8",  time(*pool8)},
            {"dfork2", time(*dfork2)}, {"dfork4", time(*dfork4)}, {"dfork8", time(*dfork8)},
        };
        for (const auto& r : rows)
            std::println("{:<16} {:>8} {:>11.1f} {:>8.2f}x {:>8.2f}x",
                         r.label, n, r.ns / n, p1 / r.ns, a / r.ns);
        std::println("");
    }

    return (g_sink == 0x1234567u) ? 1 : 0;
}
