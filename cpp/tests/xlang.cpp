// Cross-language check, C++ side: evaluate every line of the given files with
// the first registered strategy and print one result per line —
//   E                  rejected (threw)
//   nan                NaN (sign and payload differ between strategies)
//   <16 hex digits>    the IEEE-754 bits of the value
// bench/xlang_compare.py compares this output with the other three
// languages'. Within a language every strategy shares one lexer, so the
// in-language fuzz cannot see a lexer bug; only this comparison can.
#include "parser/evaluator.hpp"
#include "test_util.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>

using namespace mp;

int main(int argc, char** argv) {
    const auto env = mp::test::testEnv();
    auto evs = all_evaluators();
    IEvaluator& ev = *evs.front();
    for (int k = 1; k < argc; ++k) {
        std::ifstream in(argv[k]);
        if (!in) { std::println(stderr, "cannot open {}", argv[k]); return 2; }
        for (std::string line; std::getline(in, line);) {
            double v = 0;
            try { v = ev.eval(line, env.data()); } catch (...) { std::println("E"); continue; }
            if (std::isnan(v)) { std::println("nan"); continue; }
            std::println("{:016x}", std::bit_cast<std::uint64_t>(v));
        }
    }
    return 0;
}
