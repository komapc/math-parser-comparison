#include "parser/lexer.hpp"
#include "parser/parser.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// In one sentence: parsing driven by each operator's binding power, looping while
// the next operator binds tighter than the caller's minimum.
//
// Strategy 3: Pratt / precedence climbing.
// Each token has a binding power; nud() starts an expression (number, prefix
// +/-, parens), led-style infix handling lives in parseExpr(minbp). Associativity
// is an asymmetry in the binding powers. Binding powers:
//   + -     : 10
//   * /     : 20
//   prefix  : 30
//   ^       : 40  (right-associative: recurse with lbp-1)
int infixLbp(TokenType t) {
    switch (t) {
        case TokenType::Plus:
        case TokenType::Minus: return 10;
        case TokenType::Star:
        case TokenType::Slash: return 20;
        case TokenType::Caret: return 40;
        default:               return 0;  // not an infix operator
    }
}
constexpr int kPrefixBp = 30;

class Pratt final : public IParser {
public:
    const char* name() const override { return "pratt"; }

    ExprPtr parse(std::string_view src) override {
        lx_ = Lexer(src);
        cur_ = lx_.next();
        ExprPtr e = parseExpr(0);
        if (peek().type != TokenType::End) {
            throw std::runtime_error("unexpected token at position " +
                                     std::to_string(peek().pos));
        }
        return e;
    }

private:
    // Streaming lexer + one token of lookahead; no token array.
    Lexer lx_;
    Token cur_;

    const Token& peek() const { return cur_; }
    Token advance() { const Token t = cur_; cur_ = lx_.next(); return t; }

    ExprPtr nud() {
        Token t = advance();
        switch (t.type) {
            case TokenType::Number:
                return number(t.value);
            case TokenType::Ident:
                return variable(static_cast<int>(t.value));
            case TokenType::Plus:
            case TokenType::Minus:
                return unary(t.type, parseExpr(kPrefixBp));
            case TokenType::LParen: {
                ExprPtr e = parseExpr(0);
                if (peek().type != TokenType::RParen) {
                    throw std::runtime_error("expected ')' at position " +
                                             std::to_string(peek().pos));
                }
                advance();
                return e;
            }
            default:
                throw std::runtime_error(
                    "expected number, '(' or unary operator at position " +
                    std::to_string(t.pos));
        }
    }

    ExprPtr parseExpr(int minbp) {
        ExprPtr left = nud();
        for (;;) {
            const TokenType t = peek().type;
            const int lbp = infixLbp(t);
            if (lbp == 0 || lbp <= minbp) break;
            advance();
            const bool rightAssoc = (t == TokenType::Caret);
            const int nextMin = rightAssoc ? lbp - 1 : lbp;
            ExprPtr right = parseExpr(nextMin);
            left = binary(t, std::move(left), std::move(right));
        }
        return left;
    }
};

}  // namespace

std::unique_ptr<IParser> make_pratt() {
    return std::make_unique<Pratt>();
}

}  // namespace mp
