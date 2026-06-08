// Benchmark harness comparing every evaluation strategy on one-shot
// string -> value:
//
//   * Correctness pass: all strategies must agree on a large random corpus.
//   * Timing table:      one-shot ns/expr and ns/leaf across expression sizes.
#include "parser/evaluator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <print>
#include <random>
#include <string>
#include <vector>

using namespace mp;

namespace {

using Clock = std::chrono::steady_clock;

// Sink to defeat dead-code elimination.
volatile std::uint64_t g_sink = 0;

// ---- random valid-expression generator (deterministic) ---------------------
class Generator {
public:
    explicit Generator(std::uint32_t seed) : rng_(seed) {}

    // Build an expression string with exactly `leaves` numeric operands.
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
            out += ' ';
            out += op();
            out += ' ';
            gen(n - l, out);
        }
        if (paren) out += ')';
    }
};

std::vector<std::string> makeCorpus(int leaves, int count, std::uint32_t seed) {
    Generator g(seed);
    std::vector<std::string> corpus;
    corpus.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) corpus.push_back(g.make(leaves));
    return corpus;
}

bool sameResult(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == b) return true;  // exact, incl. +/-inf
    const double d = std::fabs(a - b);
    return d <= 1e-6 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

// Run `f` `reps` times, return the fastest wall time in nanoseconds.
double bestNs(int reps, const std::function<void()>& f) {
    double best = std::numeric_limits<double>::infinity();
    for (int r = 0; r < reps; ++r) {
        const auto t0 = Clock::now();
        f();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    return best;
}

// ---- correctness pass ------------------------------------------------------
void correctnessPass(const std::vector<std::vector<std::string>>& corpora) {
    auto evs = all_evaluators();
    std::size_t total = 0, mismatches = 0;
    for (const auto& corpus : corpora) {
        for (const auto& expr : corpus) {
            ++total;
            const double ref = evs.front()->eval(expr);
            for (std::size_t i = 1; i < evs.size(); ++i) {
                if (!sameResult(ref, evs[i]->eval(expr))) {
                    ++mismatches;
                    if (mismatches <= 5) {
                        std::println("  MISMATCH on \"{}\": {}={:g} vs {}={:g}",
                                    expr, evs.front()->name(), ref,
                                    evs[i]->name(), evs[i]->eval(expr));
                    }
                    break;
                }
            }
        }
    }
    std::println("Correctness: {}/{} expressions agree across all strategies{}\n",
                total - mismatches, total, mismatches ? "  <-- FAILURES" : "");
}

// ---- timing table ----------------------------------------------------------
void timingTable(const std::vector<int>& sizes,
                 const std::vector<std::vector<std::string>>& corpora,
                 int reps) {
    auto evs = all_evaluators();

    std::println("One-shot: source text -> value (single evaluation)");
    std::println("{:<26} {:>8} {:>8} {:>16} {:>16}",
                "strategy", "leaves", "exprs", "ns/expr", "ns/leaf");
    std::println("{}", std::string(78, '-'));

    for (std::size_t si = 0; si < sizes.size(); ++si) {
        const int leaves = sizes[si];
        const auto& corpus = corpora[si];
        const auto count = static_cast<double>(corpus.size());

        for (auto& ev : evs) {
            const double ns = bestNs(reps, [&] {
                double acc = 0;
                for (const auto& e : corpus) acc += ev->eval(e);
                g_sink += static_cast<std::uint64_t>(acc);
            });
            std::println("{:<26} {:>8} {:>8.0f} {:>16.1f} {:>16.2f}",
                        ev->name(), leaves, count, ns / count, ns / count / leaves);
        }
        std::println("");
    }
}

}  // namespace

int main() {
    const std::vector<int> sizes = {10, 100, 1000, 10000};
    const int reps = 10;

    // Roughly constant total work per size (~100k leaves).
    std::vector<std::vector<std::string>> corpora;
    corpora.reserve(sizes.size());
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const int count = std::max(4, 100000 / sizes[i]);
        corpora.push_back(makeCorpus(sizes[i], count,
                                     0xC0FFEEu + static_cast<std::uint32_t>(i)));
    }

    std::println("== math-expression evaluator comparison ==\n");
    correctnessPass(corpora);
    timingTable(sizes, corpora, reps);

    return (g_sink == 0x1234567u) ? 1 : 0;
}
