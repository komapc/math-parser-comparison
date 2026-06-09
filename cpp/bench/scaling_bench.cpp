// Batch-throughput scaling: run the shared corpus across W worker threads and
// see how each strategy's throughput scales at W = 1, 2, 4. The question is not
// "which is fastest" (that's corpus_bench) but "do the gaps change with cores"
// — i.e. do allocation-heavy strategies scale *worse* under shared-allocator
// contention? Each thread gets its OWN evaluator (they hold mutable state).
//
// Usage: ./build/scaling_bench [corpus_dir]   (default: bench/corpus)
#include "parser/evaluator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <vector>

using namespace mp;
using Clock = std::chrono::steady_clock;

namespace {
volatile std::uint64_t g_sink = 0;
constexpr int kSizeN = 1000;       // use the 1000-leaf corpus
constexpr int kRepeat = 60;        // replicate corpus so there is enough work
const std::vector<int> kWorkers = {1, 2, 4};

std::vector<std::string> load(const std::string& dir, int n) {
    std::vector<std::string> out;
    std::ifstream f(dir + "/n" + std::to_string(n) + ".txt");
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) out.push_back(line);
    return out;
}

std::unique_ptr<IEvaluator> makeOne(std::size_t s) {
    auto v = all_evaluators();
    return std::move(v[s]);
}

double bestNs(int reps, auto f) {
    double best = std::numeric_limits<double>::infinity();
    for (int r = 0; r < reps; ++r) {
        const auto t0 = Clock::now();
        f();
        best = std::min(best, std::chrono::duration<double, std::nano>(
                                  Clock::now() - t0).count());
    }
    return best;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "bench/corpus";
    const auto corpus = load(dir, kSizeN);
    if (corpus.empty()) {
        std::println("error: missing corpus n{} in {} (run gen_corpus.py)", kSizeN, dir);
        return 1;
    }

    const std::size_t total = corpus.size() * kRepeat;       // virtual work items
    const double leaves = static_cast<double>(total) * kSizeN;

    auto names = [] {
        std::vector<std::string> ns;
        for (auto& e : all_evaluators()) ns.push_back(e->name());
        return ns;
    }();

    std::println("== C++: batch-throughput scaling (hardware_concurrency={}) ==\n",
                 std::thread::hardware_concurrency());
    std::println("ns/leaf at W workers, and speedup@4 = (ns@1 / ns@4), eff = speedup/4\n");
    std::println("{:<26}{:>10}{:>10}{:>10}{:>12}{:>8}",
                 "strategy", "W=1", "W=2", "W=4", "speedup@4", "eff");
    std::println("{}", std::string(26 + 50, '-'));

    for (std::size_t s = 0; s < names.size(); ++s) {
        std::vector<double> nsAt;
        for (int W : kWorkers) {
            std::vector<std::unique_ptr<IEvaluator>> evs;
            for (int w = 0; w < W; ++w) evs.push_back(makeOne(s));

            const double ns = bestNs(3, [&] {
                std::vector<std::thread> th;
                std::vector<double> partial(static_cast<std::size_t>(W), 0.0);
                for (int w = 0; w < W; ++w) {
                    th.emplace_back([&, w] {
                        double acc = 0;
                        for (std::size_t i = static_cast<std::size_t>(w);
                             i < total; i += static_cast<std::size_t>(W))
                            acc += evs[static_cast<std::size_t>(w)]->eval(
                                corpus[i % corpus.size()]);
                        partial[static_cast<std::size_t>(w)] = acc;
                    });
                }
                for (auto& t : th) t.join();
                for (double p : partial) g_sink += static_cast<std::uint64_t>(p);
            });
            nsAt.push_back(ns / leaves);
        }
        const double sp = nsAt.front() / nsAt.back();
        std::println("{:<26}{:>10.1f}{:>10.1f}{:>10.1f}{:>11.2f}x{:>8.2f}",
                     names[s], nsAt[0], nsAt[1], nsAt[2], sp, sp / kWorkers.back());
    }
    return (g_sink == 0x1234567u) ? 1 : 0;
}
