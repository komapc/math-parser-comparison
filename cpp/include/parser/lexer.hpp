#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>
#include <vector>

#include "parser/token.hpp"

namespace mp {

// Shared lexer used by every parsing strategy, so the comparison measures the
// parsing approach rather than differences in lexing.
//
// Two ways to consume it, one set of rules:
//   * Lexer::next()  — streaming: one token at a time, nothing materialised.
//                      For strategies that read left to right with bounded
//                      lookahead (recursive descent, shunting-yard, Pratt, the
//                      fused bottom-up reducer, the bytecode compiler).
//   * tokenize()     — the whole token array, built by calling next() until
//                      End. For strategies whose algorithm needs random access
//                      to the tokens (the divide-and-conquer multipass family,
//                      the buffered bottom-up reducer, the parallel variants).
// Both throw std::runtime_error on an unrecognised character or a malformed
// number; next() returns End for every call once the input is exhausted.
class Lexer {
public:
    Lexer() = default;
    explicit Lexer(std::string_view src)
        : p_(src.data()), end_(src.data() + src.size()), base_(src.data()) {}

    Token next() {
        for (;;) {
            if (p_ == end_) return Token{TokenType::End, pos(), 0.0};
            const char c = *p_;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p_;
                continue;
            }

            Token t;
            t.pos = pos();

            if ((c >= '0' && c <= '9') || c == '.') {
                double value = 0.0;
                auto [ptr, ec] = std::from_chars(p_, end_, value);
                if (ec == std::errc::result_out_of_range) {
                    value = outOfRange(p_, ptr);
                    ec = std::errc();
                }
                if (ec != std::errc() || ptr == p_) fail("invalid number", t.pos);
                t.type = TokenType::Number;
                t.value = value;
                p_ = ptr;
                return t;
            }

            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
                // Maximal identifier run. Only single-letter variables a-z/A-Z are
                // supported for now (multi-char names are reserved for functions).
                const char* q = p_;
                while (q < end_ && ((*q >= 'a' && *q <= 'z') ||
                                    (*q >= 'A' && *q <= 'Z') ||
                                    (*q >= '0' && *q <= '9') || *q == '_')) {
                    ++q;
                }
                if (q - p_ != 1 || c == '_') fail("unknown identifier", t.pos);
                t.type = TokenType::Ident;
                t.value = static_cast<double>((c | 0x20) - 'a');  // a-z -> 0-25
                p_ = q;
                return t;
            }

            switch (c) {
                case '+': t.type = TokenType::Plus;   break;
                case '-': t.type = TokenType::Minus;  break;
                case '*': t.type = TokenType::Star;   break;
                case '/': t.type = TokenType::Slash;  break;
                case '^': t.type = TokenType::Caret;  break;
                case '(': t.type = TokenType::LParen; break;
                case ')': t.type = TokenType::RParen; break;
                default:  failChar(c, t.pos);
            }
            ++p_;
            return t;
        }
    }

private:
    const char* p_    = nullptr;
    const char* end_  = nullptr;
    const char* base_ = nullptr;

    std::uint32_t pos() const { return static_cast<std::uint32_t>(p_ - base_); }

    // Cold paths live out of line (lexer.cpp) so the hot path that gets
    // inlined into every streaming parser stays small.
    [[noreturn, gnu::cold]] static void fail(const char* what, std::uint32_t pos);
    [[noreturn, gnu::cold]] static void failChar(char c, std::uint32_t pos);
    // Overflow/underflow is a value, not a syntax error — Python and Haskell
    // give inf / 0 here, and so do we. from_chars leaves the value unset on
    // ERANGE; strtod supplies the IEEE result (HUGE_VAL or 0) for the lexeme
    // from_chars already delimited.
    [[gnu::cold]] static double outOfRange(const char* first, const char* last);
};

// The whole token array. Always ends with a TokenType::End token.
std::vector<Token> tokenize(std::string_view src);

}  // namespace mp
