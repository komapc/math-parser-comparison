// Single-expression parallel benchmark.
//
// Measures how parse latency for ONE expression scales with thread count.
// This is distinct from scaling_bench (batch throughput) and corpus_bench (one-shot).
//
// Correctness preflight:
//   par1 vs ast-arena: within 1e-6 relative (expected: FP re-association drift).
//   par2/4/8 vs par1:  exact bit equality (same tree, only scheduling differs).
//
// Timing:
//   For each expression size, par1/2/4/8 are timed and compared to ast-arena.
//   Speedup is reported relative to par1 (not ast-arena) so the thread-count
//   scaling is visible independently of the sequential overhead.
#include "parser/evaluator.hpp"

#include <algorithm>
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
    const std::vector<int> sizes  = {100, 1000, 10000, 100000};
    const std::vector<int> repsV  = { 20,   20,    10,       5};
    const std::vector<int> countV = {  8,    8,     4,       2};

    auto arena   = make_ast_arena();
    auto par1    = make_multipass_par1();
    auto par2    = make_multipass_par2();
    auto par4    = make_multipass_par4();
    auto par8    = make_multipass_par8();

    std::println("== single-expression parallel benchmark ==\n");

    // ---- correctness preflight ---------------------------------------------
    // par1 vs ast-arena: within 1e-6 (FP re-association from middle-split is OK).
    // par2/4/8 vs par1: bit-identical (same tree shape, same eval order).
    auto bitEq = [](double a, double b) {
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    };
    std::println("Correctness preflight:");
    int errors = 0;
    Generator g(0xDEAD);
    for (int n : {10, 100, 1000}) {
        for (int i = 0; i < 200; ++i) {
            const std::string expr = g.make(n);
            const double ref  = arena->eval(expr);
            const double v1   = par1->eval(expr);
            const double v2   = par2->eval(expr);
            const double v4   = par4->eval(expr);
            const double v8   = par8->eval(expr);
            if (!sameResult(ref, v1)) {
                ++errors;
                if (errors <= 3)
                    std::println("  par1 vs arena mismatch: \"{}\" arena={:g} par1={:g}",
                                 expr, ref, v1);
            }
            // par2/4/8 must be bit-identical to par1 (same tree, only scheduling differs)
            if (!bitEq(v2, v1) || !bitEq(v4, v1) || !bitEq(v8, v1)) {
                ++errors;
                if (errors <= 3)
                    std::println("  par threading mismatch: \"{}\" par1={:g} par2={:g} par4={:g} par8={:g}",
                                 expr, v1, v2, v4, v8);
            }
        }
    }
    std::println("  {} expressions, {} errors\n", 3 * 200, errors);

    // ---- timing ------------------------------------------------------------
    std::println("{:<18} {:>8} {:>12} {:>10} {:>10}",
                 "strategy", "leaves", "ns/leaf", "×par1", "×arena");
    std::println("{}", std::string(62, '-'));

    for (std::size_t si = 0; si < sizes.size(); ++si) {
        const int n     = sizes[si];
        const int kReps = repsV[si];
        const int kCnt  = countV[si];

        std::vector<std::string> corpus;
        Generator gc(0xCAFE + static_cast<std::uint32_t>(n));
        for (int i = 0; i < kCnt; ++i) corpus.push_back(gc.make(n));

        auto time = [&](IEvaluator& ev) {
            return bestNs(kReps, [&] {
                double acc = 0;
                for (const auto& e : corpus) acc += ev.eval(e);
                g_sink += static_cast<std::uint64_t>(acc);
            }) / static_cast<double>(corpus.size());
        };

        const double nsArena = time(*arena);
        const double nsPar1  = time(*par1);
        const double nsPar2  = time(*par2);
        const double nsPar4  = time(*par4);
        const double nsPar8  = time(*par8);

        std::println("{:<18} {:>8} {:>12.1f} {:>10} {:>10.2f}",
                     arena->name(), n, nsArena / n, "—", 1.0);
        for (auto [label, ns] : {
                std::pair{"par1",  nsPar1},
                std::pair{"par2",  nsPar2},
                std::pair{"par4",  nsPar4},
                std::pair{"par8",  nsPar8}}) {
            std::println("{:<18} {:>8} {:>12.1f} {:>9.2f}× {:>9.2f}×",
                         label, n, ns / n, nsPar1 / ns, nsArena / ns);
        }
        std::println("");
    }

    return (g_sink == 0x1234567u) ? 1 : 0;
}
