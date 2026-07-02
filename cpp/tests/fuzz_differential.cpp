// Differential fuzz: every strategy must agree with every other on randomly
// generated expressions — on the value for well-formed inputs, and on
// throw-vs-accept for mutated (usually malformed) ones. Deterministic seed,
// so failures reproduce. This is what backs the "all twelve strategies
// implement one specification" claim; the hand-written cases in
// test_parsers.cpp document the specification, this enforces it in bulk.
#include "parser/evaluator.hpp"

#include "test_util.hpp"

#include <print>
#include <random>
#include <string>

using namespace mp;

namespace {

std::mt19937 rng(20260702);

std::string gen(int depth = 0) {
    std::uniform_real_distribution<> u(0, 1);
    const double r = u(rng);
    if (depth > 6 || r < 0.30) {
        if (u(rng) < 0.3) return std::string(1, static_cast<char>('a' + rng() % 4));
        static const char* lits[] = {"1", "2", "3", "0.5", "10", "7"};
        return lits[rng() % 6];
    }
    if (r < 0.42) return "-" + gen(depth + 1);
    if (r < 0.48) return "+" + gen(depth + 1);
    if (r < 0.60) return "(" + gen(depth + 1) + ")";
    static const char* ops[] = {"+", "-", "*", "/", "^", "^"};
    return gen(depth + 1) + " " + ops[rng() % 6] + " " + gen(depth + 1);
}

// Random single-character mutation: delete, insert, or duplicate. The result
// is usually malformed; the invariant is only that all strategies agree.
std::string mutate(std::string s) {
    static const char kInsert[] = "1a+-*/^()... ";
    if (s.empty()) return "(";
    switch (rng() % 3) {
        case 0: s.erase(rng() % s.size(), 1); break;
        case 1: s.insert(rng() % (s.size() + 1), 1,
                         kInsert[rng() % (sizeof(kInsert) - 1)]); break;
        default: {
            const std::size_t i = rng() % s.size();
            s.insert(i, 1, s[i]); break;
        }
    }
    return s;
}

}  // namespace

int main() {
    const auto env = mp::test::testEnv();
    const auto evs = all_evaluators();
    int mismatches = 0;

    const auto tryEval = [&](IEvaluator& ev, const std::string& e,
                             double& v) -> bool /*threw*/ {
        try { v = ev.eval(e, env.data()); return false; } catch (...) { return true; }
    };
    const auto checkAll = [&](const std::string& e) {
        double base = 0;
        const bool baseThrew = tryEval(*evs.front(), e, base);
        for (std::size_t k = 1; k < evs.size(); ++k) {
            double v = 0;
            const bool threw = tryEval(*evs[k], e, v);
            if (threw != baseThrew || (!threw && !mp::test::nearly(v, base))) {
                ++mismatches;
                std::println("MISMATCH [{}] on \"{}\": base={:g}(threw={}) got={:g}(threw={})",
                             evs[k]->name(), e, base, baseThrew, v, threw);
            }
        }
    };

    constexpr int kWellFormed = 3000;
    constexpr int kMutated = 3000;
    for (int t = 0; t < kWellFormed; ++t) checkAll(gen());
    for (int t = 0; t < kMutated; ++t) checkAll(mutate(gen()));

    std::println("{} well-formed + {} mutated exprs x {} strategies, {} mismatch(es)",
                 kWellFormed, kMutated, evs.size(), mismatches);
    return mismatches == 0 ? 0 : 1;
}
