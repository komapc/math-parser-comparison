#include "parser/lexer.hpp"

#include <charconv>
#include <stdexcept>
#include <string>

namespace mp {

std::vector<Token> tokenize(std::string_view src) {
    std::vector<Token> out;
    out.reserve(src.size() / 2 + 1);  // rough upper bound: every other char is a token
    const std::size_t n = src.size();
    std::size_t i = 0;

    while (i < n) {
        const char c = src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }

        Token t;
        t.pos = i;

        if ((c >= '0' && c <= '9') || c == '.') {
            const char* first = src.data() + i;
            const char* last = src.data() + n;
            double value = 0.0;
            auto [ptr, ec] = std::from_chars(first, last, value);
            if (ec != std::errc() || ptr == first) {
                throw std::runtime_error("invalid number at position " +
                                         std::to_string(i));
            }
            t.type = TokenType::Number;
            t.value = value;
            i += static_cast<std::size_t>(ptr - first);
            out.push_back(t);
            continue;
        }

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            // Maximal identifier run. Only single-letter variables a-z/A-Z are
            // supported for now (multi-char names are reserved for functions).
            std::size_t j = i;
            while (j < n && ((src[j] >= 'a' && src[j] <= 'z') ||
                             (src[j] >= 'A' && src[j] <= 'Z') ||
                             (src[j] >= '0' && src[j] <= '9') || src[j] == '_')) {
                ++j;
            }
            if (j - i != 1 || c == '_') {
                throw std::runtime_error("unknown identifier at position " +
                                         std::to_string(i));
            }
            t.type = TokenType::Ident;
            t.value = static_cast<double>((c | 0x20) - 'a');  // a-z -> 0-25
            i = j;
            out.push_back(t);
            continue;
        }

        switch (c) {
            case '+': t.type = TokenType::Plus;   break;
            case '-': t.type = TokenType::Minus;  break;
            case '*': t.type = TokenType::Star;   break;
            case '/': t.type = TokenType::Slash;  break;
            case '^': t.type = TokenType::Caret;  break;
            case '(': t.type = TokenType::LParen; break;
            case ')': t.type = TokenType::RParen; break;
            default:
                throw std::runtime_error(
                    std::string("unexpected character '") + c +
                    "' at position " + std::to_string(i));
        }
        ++i;
        out.push_back(t);
    }

    out.push_back(Token{TokenType::End, 0.0, n});
    return out;
}

}  // namespace mp
