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
    return std::strtod(std::string(first, last).c_str(), nullptr);
}

}  // namespace mp
