#pragma once

#include <cstdint>

namespace mp {

enum class TokenType : std::uint8_t {
    Number,
    Ident,   // single-letter variable a-z (index in Token::value)
    Plus,
    Minus,
    Star,
    Slash,
    Caret,   // exponentiation: ^
    LParen,
    RParen,
    End,     // sentinel: end of input
};

// 16 bytes: the type and the byte offset share one 8-byte word ahead of the
// value, so an array of tokens is two cache lines per eight tokens instead of
// three. Sources over 4 GB are not a concern for a benchmark harness.
struct Token {
    TokenType     type;
    std::uint32_t pos   = 0;    // byte offset in source, for diagnostics
    double        value = 0.0;  // Number: the literal; Ident: variable index (0-25)
};

}  // namespace mp
