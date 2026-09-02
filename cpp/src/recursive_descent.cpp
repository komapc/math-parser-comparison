#include "parser/lexer.hpp"
#include "parser/parser.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// In one sentence: a function per grammar rule calls down the precedence ladder,
// building an AST as the recursion returns.
//
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
        lx_ = Lexer(src);
        cur_ = lx_.next();
        ExprPtr e = parseExpr();
        expect(TokenType::End);
        return e;
    }

private:
    // Streaming: the lexer hands over one token at a time; cur_ is the single
    // token of lookahead this grammar needs. No token array is built.
    Lexer lx_;
    Token cur_;

    const Token& peek() const { return cur_; }
    Token take() { const Token t = cur_; cur_ = lx_.next(); return t; }
    TokenType advance() { return take().type; }
    bool check(TokenType t) const { return peek().type == t; }

    void expect(TokenType t) {
        if (!check(t)) {
            throw std::runtime_error("unexpected token at position " +
                                     std::to_string(peek().pos));
        }
        take();
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
            return number(take().value);
        }
        if (check(TokenType::Ident)) {
            return variable(static_cast<int>(take().value));
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
