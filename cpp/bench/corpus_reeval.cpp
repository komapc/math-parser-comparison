// Cross-language reeval: compile once, evaluate many against changing variables,
// reading the SHARED variable corpus (bench/corpus/vars_n*.txt) so C++/Python/
// Haskell measure byte-identical expressions. Reports compile + per-eval ns/expr
// and the break-even eval count vs re-parsing.
//
// Usage: ./build/corpus_reeval [corpus_dir]   (default: bench/corpus)
#include "parser/reeval.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <limits>
#include <print>
#include <random>
#include <string>
#include <vector>

using namespace mp;
using Clock = std::chrono::steady_clock;

namespace {
volatile std::uint64_t g_sink = 0;
const std::vector<int> kSizes = {10, 100, 1000};

std::vector<std::string> load(const std::string& dir, int n) {
    std::vector<std::string> out;
    std::ifstream f(dir + "/vars_n" + std::to_string(n) + ".txt");
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) out.push_back(line);
    return out;
}

std::vector<std::array<double, kNumVars>> makeEnvs(int n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(0.5, 2.0);
    std::vector<std::array<double, kNumVars>> envs(static_cast<std::size_t>(n));
    for (auto& e : envs) for (auto& v : e) v = dist(rng);
    return envs;
}

double bestNs(int reps, auto f) {
    double best = std::numeric_limits<double>::infinity();
    for (int r = 0; r < reps; ++r) {
        const auto t0 = Clock::now();
        f();
        best = std::min(best, std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
    }
    return best;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "bench/corpus";
    const int reps = 20;
    const auto envs = makeEnvs(8, 0x5EED);

    std::println("== C++: re-evaluation on shared corpus (compile once, eval many) ==\n");
    std::println("Variables a-d, values in [0.5, 2.0]");
    std::println("{:<16}{:>8}{:>8}{:>16}{:>16}{:>12}",
                 "strategy", "leaves", "exprs", "compile ns", "per-eval ns", "break-even");
    std::println("{}", std::string(76, '-'));

    for (int n : kSizes) {
        const auto corpus = load(dir, n);
        if (corpus.empty()) {
            std::println("error: missing vars_n{} in {} (run gen_corpus.py)", n, dir);
            return 1;
        }
        const auto count = static_cast<double>(corpus.size());
        auto compilers = all_compilers();

        struct Row { std::string name; double compileNs, evalNs; };
        std::vector<Row> rows;
        double reparseEval = 0.0;

        for (auto& c : compilers) {
            const double compileNs = bestNs(reps, [&] {
                for (const auto& e : corpus) {
                    auto ce = c->compile(e);
                    g_sink += reinterpret_cast<std::uintptr_t>(ce.get());
                }
            });
            std::vector<std::unique_ptr<ICompiledExpr>> compiled;
            for (const auto& e : corpus) compiled.push_back(c->compile(e));
            const double evalNs = bestNs(reps, [&] {
                double acc = 0; std::size_t ei = 0;
                for (const auto& ce : compiled) {
                    acc += ce->eval(envs[ei].data());
                    ei = (ei + 1) % envs.size();
                }
                g_sink += static_cast<std::uint64_t>(acc);
            });
            rows.push_back({c->name(), compileNs / count, evalNs / count});
            if (std::string(c->name()) == "reparse-rd") reparseEval = evalNs / count;
        }

        for (const auto& r : rows) {
            const double denom = reparseEval - r.evalNs;
            const std::string be = (r.name == "reparse-rd") ? "(baseline)"
                                 : (denom <= 0) ? "n/a"
                                 : std::format("{:.1f}", r.compileNs / denom);
            std::println("{:<16}{:>8}{:>8.0f}{:>16.1f}{:>16.2f}{:>12}",
                         r.name, n, count, r.compileNs, r.evalNs, be);
        }
        std::println("");
    }
    return (g_sink == 0x1234567u) ? 1 : 0;
}
