// Parallel batch benchmark: measures how batch throughput (ns/expr) scales
// from 1 to 8 threads for arena-based strategies.
//
// Each thread owns its own evaluator instance (no sharing, no locks).
// The batch is divided into equal chunks; wall time spans first-launch to
// last-join. "Best of N reps" removes scheduling outliers.
#include "parser/evaluator.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace mp;

namespace {

using Clock = std::chrono::steady_clock;
volatile std::uint64_t g_sink = 0;

// ---- deterministic expression generator (matches benchmark.cpp) ------------
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

std::vector<std::string> makeCorpus(int leaves, int count, std::uint32_t seed) {
    Generator g(seed);
    std::vector<std::string> c;
    c.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) c.push_back(g.make(leaves));
    return c;
}

// ---- batch parallel timing -------------------------------------------------
// Divides corpus across T threads; each thread uses its own evaluator.
// Returns best-of-reps total wall ns.
double batchNs(const std::vector<std::string>& corpus,
               const std::function<std::unique_ptr<IEvaluator>()>& make_ev,
               unsigned threads, int reps) {
    const std::size_t n = corpus.size();
    const unsigned t = std::max(1u, threads);
    const std::size_t chunk = (n + t - 1) / t;

    double best = std::numeric_limits<double>::infinity();
    for (int r = 0; r < reps; ++r) {
        std::vector<std::thread> pool;
        pool.reserve(t);
        const auto t0 = Clock::now();
        for (unsigned w = 0; w < t; ++w) {
            const std::size_t lo = std::min(n, static_cast<std::size_t>(w) * chunk);
            const std::size_t hi = std::min(n, lo + chunk);
            if (lo >= hi) break;
            pool.emplace_back([&corpus, &make_ev, lo, hi]() {
                auto ev = make_ev();
                double acc = 0;
                for (std::size_t i = lo; i < hi; ++i) acc += ev->eval(corpus[i]);
                g_sink += static_cast<std::uint64_t>(acc);
            });
        }
        for (auto& th : pool) th.join();
        const double ns =
            std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
        best = std::min(best, ns);
    }
    return best;
}

}  // namespace

int main() {
    const int kLeaves = 1000;
    const int kCount  = 400;
    const int kReps   = 5;
    const unsigned kThreads[] = {1, 2, 4, 8};

    const auto corpus = makeCorpus(kLeaves, kCount, 0xBABE);

    struct Strategy {
        const char* name;
        std::function<std::unique_ptr<IEvaluator>()> make;
    };
    const Strategy strategies[] = {
        {"ast-arena",       make_ast_arena},
        {"multipass-arena", make_ast_multipass_arena},
    };

    std::println("== parallel batch benchmark ({} × {}-leaf exprs, best of {}) ==\n",
                 kCount, kLeaves, kReps);
    std::println("{:<18} {:>5} {:>14} {:>12}",
                 "strategy", "T", "ns/expr", "speedup");
    std::println("{}", std::string(54, '-'));

    for (const auto& s : strategies) {
        double base = 0;
        for (const unsigned t : kThreads) {
            const double ns  = batchNs(corpus, s.make, t, kReps) / kCount;
            if (t == 1) base = ns;
            std::println("{:<18} {:>5} {:>14.0f} {:>10.1f}×",
                         s.name, t, ns, base / ns);
        }
        std::println("");
    }

    return (g_sink == 0x1234567u) ? 1 : 0;
}
