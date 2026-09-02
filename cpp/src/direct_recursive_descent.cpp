#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// In one sentence: recursive descent whose rules return the computed number
// directly, so no AST is ever built.
//
// Direct recursive descent: same grammar as src/recursive_descent.cpp, but each
// rule returns a double computed inline -- no AST is ever built.
class DirectRecursiveDescent final : public IEvaluator {
public:
    const char* name() const override { return "direct-recursive-descent"; }

    double eval(std::string_view src, const double* vars = nullptr) override {
        lx_ = Lexer(src);
        cur_ = lx_.next();
        vars_ = vars;
        const double v = expr();
        if (!check(TokenType::End)) {
            throw std::runtime_error("unexpected token at position " +
                                     std::to_string(peek().pos));
        }
        return v;
    }

private:
    // Streaming lexer + one token of lookahead; no token array.
    Lexer         lx_;
    Token         cur_;
    const double* vars_ = nullptr;

    const Token& peek() const { return cur_; }
    Token take() { const Token t = cur_; cur_ = lx_.next(); return t; }
    bool check(TokenType t) const { return peek().type == t; }

    double expr() {
        double l = term();
        while (check(TokenType::Plus) || check(TokenType::Minus)) {
            const TokenType op = take().type;
            const double r = term();
            l = (op == TokenType::Plus) ? l + r : l - r;
        }
        return l;
    }
    double term() {
        double l = unaryRule();
        while (check(TokenType::Star) || check(TokenType::Slash)) {
            const TokenType op = take().type;
            const double r = unaryRule();
            l = (op == TokenType::Star) ? l * r : l / r;
        }
        return l;
    }
    double unaryRule() {
        if (check(TokenType::Plus) || check(TokenType::Minus)) {
            const TokenType op = take().type;
            const double v = unaryRule();
            return (op == TokenType::Minus) ? -v : v;
        }
        return power();
    }
    double power() {
        const double base = primary();
        if (check(TokenType::Caret)) {
            take();
            return std::pow(base, unaryRule());
        }
        return base;
    }
    double primary() {
        if (check(TokenType::Number)) return take().value;
        if (check(TokenType::Ident)) {
            const int idx = static_cast<int>(take().value);
            return vars_ ? vars_[idx] : 0.0;
        }
        if (check(TokenType::LParen)) {
            take();
            const double v = expr();
            if (!check(TokenType::RParen)) {
                throw std::runtime_error("expected ')' at position " +
                                         std::to_string(peek().pos));
            }
            take();
            return v;
        }
        throw std::runtime_error("expected number or '(' at position " +
                                 std::to_string(peek().pos));
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_direct_recursive_descent() {
    return std::make_unique<DirectRecursiveDescent>();
}

}  // namespace mp
