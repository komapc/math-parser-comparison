// Re-evaluation benchmark: compile once, evaluate many times against changing
// variable bindings. This is where compiled forms (AST/RPN/bytecode) earn their
// keep over re-parsing, and where the arena AST beats the pointer AST.
//
//   * Correctness: all compilers agree for several random environments.
//   * compile ns/expr: source -> compiled form.
//   * per-eval ns/expr: one evaluation of the compiled form (allocation-free).
//   * break-even: how many evaluations amortize the compile cost vs. reparsing.
#include "parser/reeval.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace mp;

namespace {

using Clock = std::chrono::steady_clock;
volatile std::uint64_t g_sink = 0;

// Generates valid arithmetic expressions whose leaves are a mix of numbers and
// variables (single letters a-d; 'e' avoided -- it reads as an exponent marker).
class VarGenerator {
public:
    explicit VarGenerator(std::uint32_t seed) : rng_(seed) {}

    std::string make(int leaves) {
        std::string out;
        out.reserve(static_cast<std::size_t>(leaves) * 5);
        gen(leaves, out);
        return out;
    }

private:
    std::mt19937 rng_;

    void leafToken(std::string& out) {
        if (std::uniform_int_distribution<int>(0, 4)(rng_) == 0) out += '-';
        if (std::uniform_int_distribution<int>(0, 1)(rng_) == 0) {
            out += static_cast<char>('a' + std::uniform_int_distribution<int>(0, 3)(rng_));
        } else {
            out += std::to_string(std::uniform_int_distribution<int>(1, 20)(rng_));
        }
    }

    char op() {
        static const char ops[] = {'+', '+', '-', '-', '*', '/'};
        return ops[std::uniform_int_distribution<int>(0, 5)(rng_)];
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
    VarGenerator g(seed);
    std::vector<std::string> c;
    c.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) c.push_back(g.make(leaves));
    return c;
}

// A few random environments (variable values in [0.5, 2.0]) to cycle through.
std::vector<std::array<double, kNumVars>> makeEnvs(int n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(0.5, 2.0);
    std::vector<std::array<double, kNumVars>> envs(static_cast<std::size_t>(n));
    for (auto& e : envs)
        for (auto& v : e) v = dist(rng);
    return envs;
}

bool sameResult(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == b) return true;
    const double d = std::fabs(a - b);
    return d <= 1e-6 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

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

void correctnessPass(const std::vector<std::vector<std::string>>& corpora,
                     const std::vector<std::array<double, kNumVars>>& envs) {
    auto compilers = all_compilers();
    std::size_t total = 0, mism = 0;
    for (const auto& corpus : corpora) {
        for (const auto& expr : corpus) {
            std::vector<std::unique_ptr<ICompiledExpr>> compiled;
            for (auto& c : compilers) compiled.push_back(c->compile(expr));
            for (const auto& env : envs) {
                ++total;
                const double ref = compiled.front()->eval(env.data());
                for (std::size_t i = 1; i < compiled.size(); ++i) {
                    if (!sameResult(ref, compiled[i]->eval(env.data()))) { ++mism; break; }
                }
            }
        }
    }
    std::printf("Correctness: %zu/%zu (expr,env) results agree across all compilers%s\n\n",
                total - mism, total, mism ? "  <-- FAILURES" : "");
}

}  // namespace

int main() {
    const std::vector<int> sizes = {10, 100, 1000};
    const int reps = 20;
    const auto envs = makeEnvs(8, 0x5EED);

    std::vector<std::vector<std::string>> corpora;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const int count = std::max(8, 40000 / sizes[i]);
        corpora.push_back(makeCorpus(sizes[i], count,
                                     0xA11CEu + static_cast<std::uint32_t>(i)));
    }

    std::printf("== re-evaluation comparison (compile once, evaluate many) ==\n\n");
    correctnessPass(corpora, envs);

    std::printf("Variables a-d, values in [0.5, 2.0]\n");
    std::printf("%-12s %8s %8s %16s %16s %12s\n",
                "strategy", "leaves", "exprs", "compile ns/expr", "per-eval ns/expr",
                "break-even");
    std::printf("%s\n", std::string(78, '-').c_str());

    auto compilers = all_compilers();

    for (std::size_t si = 0; si < sizes.size(); ++si) {
        const auto& corpus = corpora[si];
        const auto count = static_cast<double>(corpus.size());

        // Reference per-eval cost of the reparse baseline (for break-even).
        double reparseEval = 0.0;

        struct Row { std::string name; double compileNs; double evalNs; };
        std::vector<Row> rows;

        for (auto& c : compilers) {
            // compile timing: source -> compiled form
            const double compileNs = bestNs(reps, [&] {
                for (const auto& e : corpus) {
                    auto ce = c->compile(e);
                    g_sink += reinterpret_cast<std::uintptr_t>(ce.get());
                }
            });

            // per-eval timing: pre-compile once, then evaluate (allocation-free)
            std::vector<std::unique_ptr<ICompiledExpr>> compiled;
            compiled.reserve(corpus.size());
            for (const auto& e : corpus) compiled.push_back(c->compile(e));

            const double evalNs = bestNs(reps, [&] {
                double acc = 0;
                int ei = 0;
                for (const auto& ce : compiled) {
                    acc += ce->eval(envs[static_cast<std::size_t>(ei)].data());
                    ei = (ei + 1) % static_cast<int>(envs.size());
                }
                g_sink += static_cast<std::uint64_t>(acc);
            });

            rows.push_back({c->name(), compileNs / count, evalNs / count});
            if (std::string(c->name()) == "reparse-rd") reparseEval = evalNs / count;
        }

        for (const auto& r : rows) {
            // break-even N: evals after which compiling beats reparsing.
            // (compile + N*eval) < N*reparseEval  =>  N > compile / (reparseEval - eval)
            char be[24];
            const double denom = reparseEval - r.evalNs;
            if (r.name == "reparse-rd")      std::snprintf(be, sizeof be, "%s", "(baseline)");
            else if (denom <= 0)             std::snprintf(be, sizeof be, "%s", "n/a");
            else                             std::snprintf(be, sizeof be, "%.1f", r.compileNs / denom);

            std::printf("%-12s %8d %8.0f %16.1f %16.2f %12s\n",
                        r.name.c_str(), sizes[si], count, r.compileNs, r.evalNs, be);
        }
        std::printf("\n");
    }

    return (g_sink == 0x1234567u) ? 1 : 0;
}
