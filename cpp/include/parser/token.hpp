#pragma once

#include <cstddef>

namespace mp {

enum class TokenType {
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

struct Token {
    TokenType   type;
    double      value = 0.0;  // Number: the literal; Ident: variable index (0-25)
    std::size_t pos   = 0;    // byte offset in source, for diagnostics
};

}  // namespace mp
