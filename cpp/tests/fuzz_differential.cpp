// Differential fuzz: every strategy must agree with every other on randomly
// generated expressions — on the value for well-formed inputs, and on
// throw-vs-accept for mutated (usually malformed) ones. Deterministic seed,
// so failures reproduce. This is what backs the "all fifteen strategies
// implement one specification" claim; the hand-written cases in
// test_parsers.cpp document the specification, this enforces it in bulk.
#include "parser/evaluator.hpp"

#include "test_util.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <random>
#include <utility>
#include <string>
#include <vector>

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

// Long, shallow expression (hundreds of leaves): gen() alone tops out at a
// few dozen tokens, and the buffered strategies grow their stacks only here.
std::string genLong() {
    const int leaves = 200 + static_cast<int>(rng() % 1500);
    std::string s;
    for (int i = 0; i < leaves; ++i) {
        if (i) s += " +-*/"[1 + rng() % 4];
        s += (rng() % 5 == 0) ? "(" + gen(3) + ")" : gen(4);
    }
    return s;
}

// Reads every line of a file verbatim, including blank ones: some mutated
// lines are legitimately the empty string (a valid malformed-input case),
// so this must NOT drop empty lines the way corpus_bench's loader does.
std::vector<std::string> loadLines(const std::filesystem::path& p) {
    std::vector<std::string> out;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) out.push_back(line);
    return out;
}

// ctest chdir's into the test's binary directory, whose parent differs
// between a local `cpp/build` and CI's repo-root `build` — so probe a
// small set of candidates rather than assuming one relative depth.
std::filesystem::path findBenchDir() {
    namespace fs = std::filesystem;
    for (const char* cand : {"../bench", "../../bench", "bench"}) {
        if (fs::exists(fs::path(cand) / "gen_fuzz.py")) return fs::path(cand);
    }
    return {};
}

// Loads bench/fuzz/{well_formed,mutated}.txt — the corpus every other
// language's fuzz test also reads (bench/gen_fuzz.py), on top of (not
// instead of) this file's own C++-specific generator above. Generates the
// files via `python3 bench/gen_fuzz.py` if missing; if that also fails
// (e.g. no python3 in this environment), prints a note and skips the extra
// check rather than failing the whole suite over missing tooling.
std::pair<std::vector<std::string>, std::vector<std::string>> loadSharedFuzz() {
    namespace fs = std::filesystem;
    const fs::path bench = findBenchDir();
    if (bench.empty()) {
        std::println("note: bench/ not found from cwd={}; skipping shared-corpus fuzz check",
                     fs::current_path().string());
        return {};
    }
    const fs::path wf = bench / "fuzz" / "well_formed.txt";
    const fs::path mu = bench / "fuzz" / "mutated.txt";
    if (!fs::exists(wf) || !fs::exists(mu)) {
        const std::string cmd = "python3 " + (bench / "gen_fuzz.py").string();
        std::println("shared fuzz corpus missing; generating via `{}`", cmd);
        if (std::system(cmd.c_str()) != 0 || !fs::exists(wf) || !fs::exists(mu)) {
            std::println("note: could not generate shared fuzz corpus (python3 unavailable?); "
                         "skipping shared-corpus fuzz check");
            return {};
        }
    }
    return {loadLines(wf), loadLines(mu)};
}

}  // namespace

int main() {
    const auto env = mp::test::testEnv();
    auto evs = all_evaluators();
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
    constexpr int kLong = 150;  // x2: well-formed + mutated
    for (int t = 0; t < kWellFormed; ++t) checkAll(gen());
    for (int t = 0; t < kMutated; ++t) checkAll(mutate(gen()));
    for (int t = 0; t < kLong; ++t) checkAll(genLong());
    for (int t = 0; t < kLong; ++t) checkAll(mutate(genLong()));

    std::println("{} well-formed + {} mutated + {} long exprs x {} strategies, {} mismatch(es)",
                 kWellFormed, kMutated, 2 * kLong, evs.size(), mismatches);

    const auto [sharedWellFormed, sharedMutated] = loadSharedFuzz();
    for (const auto& e : sharedWellFormed) checkAll(e);
    for (const auto& e : sharedMutated) checkAll(e);
    if (!sharedWellFormed.empty() || !sharedMutated.empty()) {
        std::println("{} well-formed + {} mutated exprs (shared corpus) x {} strategies, "
                     "{} mismatch(es) total",
                     sharedWellFormed.size(), sharedMutated.size(), evs.size(), mismatches);
    }
    return mismatches == 0 ? 0 : 1;
}
