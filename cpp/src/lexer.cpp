#include "parser/lexer.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace mp {

std::vector<Token> tokenize(std::string_view src) {
    std::vector<Token> out;
    out.reserve(src.size() / 2 + 1);  // heuristic (spaced input); unspaced
                                      // input like "1+1" can exceed it and
                                      // grow the vector once more
    Lexer lx(src);
    for (;;) {
        out.push_back(lx.next());
        if (out.back().type == TokenType::End) return out;
    }
}

void Lexer::fail(const char* what, std::uint32_t pos) {
    throw std::runtime_error(std::string(what) + " at position " + std::to_string(pos));
}

void Lexer::failChar(char c, std::uint32_t pos) {
    throw std::runtime_error(std::string("unexpected character '") + c +
                             "' at position " + std::to_string(pos));
}

double Lexer::outOfRange(const char* first, const char* last) {
    // std::strtod is locale-dependent (this ERANGE fallback is the only place
    // it's used — the fast path above is std::from_chars, which is always
    // "C"-locale). A process running under a comma-decimal locale would
    // misparse a token like "1.5e400" here. Not fixed: doing so portably
    // needs strtod_l/newlocale (POSIX-only, not in the C++ standard) purely
    // to guard an out-of-range-literal edge case, which is disproportionate
    // for a benchmark harness that never changes its own locale.
    return std::strtod(std::string(first, last).c_str(), nullptr);
}

}  // namespace mp
