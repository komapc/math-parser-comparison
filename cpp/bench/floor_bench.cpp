// Serial-floor analysis for single-expression parallel MP.
//
// The direct-fork variant's time is  setup(tokenize + buildCandidates)  +
// parseRangeD (the only parallel part; eval is fused in). No amount of parse
// parallelism can beat the setup floor. This bench measures that floor against
// ast-arena's *total* to answer one question: could parallel MP ever cross
// arena on one expression, or is the serial prologue alone already a loss?
//
//   floor  <  arena  -> possible in principle (parse-scaling is the only gap)
//   floor  >= arena  -> impossible on this grammar (the prologue alone loses)
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <algorithm>
#include <chrono>
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

int main() {
    const std::vector<int> sizes = {10000, 100000};
    const std::vector<int> repsV = {   25,     12};
    const int kCnt = 4;

    auto arena  = make_ast_arena();
    auto setup  = make_mp_setup_only();
    auto dfork4 = make_multipass_dfork4();
    auto dfork8 = make_multipass_dfork8();

    std::println("== serial-floor analysis: can parallel MP ever cross arena? ==\n");
    std::println("floor = tokenize + buildCandidates (the un-parallelisable prologue)\n");
    std::println("{:<16} {:>10} {:>10} {:>12}", "component", "ns/leaf", "x arena", "note");
    std::println("{}", std::string(52, '-'));

    for (std::size_t si = 0; si < sizes.size(); ++si) {
        const int n = sizes[si], kReps = repsV[si];
        std::vector<std::string> corpus;
        Generator gc(0xF100 + static_cast<std::uint32_t>(n));
        for (int i = 0; i < kCnt; ++i) corpus.push_back(gc.make(n));

        auto timeEv = [&](IEvaluator& ev) {
            return bestNs(kReps, [&] {
                double acc = 0;
                for (const auto& e : corpus) acc += ev.eval(e);
                g_sink += static_cast<std::uint64_t>(acc);
            }) / static_cast<double>(corpus.size());
        };
        // raw tokenize alone (shared by arena and MP) for reference
        const double tok = bestNs(kReps, [&] {
            std::size_t s = 0;
            for (const auto& e : corpus) s += tokenize(e).size();
            g_sink += s;
        }) / static_cast<double>(corpus.size());

        const double a  = timeEv(*arena);
        const double fl = timeEv(*setup);
        const double d4 = timeEv(*dfork4);
        const double d8 = timeEv(*dfork8);

        std::println("n = {}", n);
        std::println("{:<16} {:>10.1f} {:>10.2f} {:>12}", "  tokenize", tok / n, tok / a, "shared");
        std::println("{:<16} {:>10.1f} {:>10.2f} {:>12}", "  FLOOR", fl / n, fl / a,
                     fl < a ? "< arena" : ">= arena");
        std::println("{:<16} {:>10.1f} {:>10.2f} {:>12}", "  dfork4", d4 / n, d4 / a, "best parallel");
        std::println("{:<16} {:>10.1f} {:>10.2f} {:>12}", "  dfork8", d8 / n, d8 / a, "");
        std::println("{:<16} {:>10.1f} {:>10.2f} {:>12}", "  ast-arena", a / n, 1.0, "target");
        std::println("  buildCandidates alone ~= FLOOR - tokenize = {:.1f} ns/leaf", (fl - tok) / n);
        std::println("");
    }

    return (g_sink == 0x1234567u) ? 1 : 0;
}
