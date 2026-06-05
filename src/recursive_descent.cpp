#include "parser/lexer.hpp"
#include "parser/parser.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// Strategy 1: recursive descent.
// One function per grammar rule; precedence is encoded in the call hierarchy.
//   expr    := term (('+'|'-') term)*
//   term    := unary (('*'|'/') unary)*
//   unary   := ('+'|'-') unary | power
//   power   := primary ('^' unary)?      (right-associative)
//   primary := NUMBER | '(' expr ')'
class RecursiveDescent final : public IParser {
public:
    const char* name() const override { return "recursive-descent"; }

    ExprPtr parse(std::string_view src) override {
        tokens_ = tokenize(src);
        pos_ = 0;
        ExprPtr e = parseExpr();
        expect(TokenType::End);
        return e;
    }

private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;

    const Token& peek() const { return tokens_[pos_]; }
    TokenType advance() { return tokens_[pos_++].type; }
    bool check(TokenType t) const { return peek().type == t; }

    void expect(TokenType t) {
        if (!check(t)) {
            throw std::runtime_error("unexpected token at position " +
                                     std::to_string(peek().pos));
        }
        ++pos_;
    }

    ExprPtr parseExpr() {
        ExprPtr left = parseTerm();
        while (check(TokenType::Plus) || check(TokenType::Minus)) {
            TokenType op = advance();
            left = binary(op, std::move(left), parseTerm());
        }
        return left;
    }

    ExprPtr parseTerm() {
        ExprPtr left = parseUnary();
        while (check(TokenType::Star) || check(TokenType::Slash)) {
            TokenType op = advance();
            left = binary(op, std::move(left), parseUnary());
        }
        return left;
    }

    ExprPtr parseUnary() {
        if (check(TokenType::Plus) || check(TokenType::Minus)) {
            TokenType op = advance();
            return unary(op, parseUnary());
        }
        return parsePower();
    }

    ExprPtr parsePower() {
        ExprPtr base = parsePrimary();
        if (check(TokenType::Caret)) {
            advance();
            // rhs via parseUnary => right-associative and allows 2^-3.
            return binary(TokenType::Caret, std::move(base), parseUnary());
        }
        return base;
    }

    ExprPtr parsePrimary() {
        if (check(TokenType::Number)) {
            return number(tokens_[pos_++].value);
        }
        if (check(TokenType::Ident)) {
            return variable(static_cast<int>(tokens_[pos_++].value));
        }
        if (check(TokenType::LParen)) {
            advance();
            ExprPtr e = parseExpr();
            expect(TokenType::RParen);
            return e;
        }
        throw std::runtime_error("expected number or '(' at position " +
                                 std::to_string(peek().pos));
    }
};

}  // namespace

std::unique_ptr<IParser> make_recursive_descent() {
    return std::make_unique<RecursiveDescent>();
}

}  // namespace mp
