#include "parser/arena_ast.hpp"

#include "parser/lexer.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace mp {
namespace {

// Recursive-descent builder that emits nodes into a flat vector and returns the
// index of each subtree's root. Same grammar as src/recursive_descent.cpp.
class Builder {
public:
    explicit Builder(std::string_view src) : tokens_(tokenize(src)) {
        nodes_.reserve(tokens_.size());
    }

    int build() {
        const int root = expr();
        if (!check(TokenType::End)) {
            throw std::runtime_error("unexpected token at position " +
                                     std::to_string(peek().pos));
        }
        return root;
    }

    std::vector<ArenaAst::Node> take() { return std::move(nodes_); }

private:
    std::vector<Token> tokens_;
    std::vector<ArenaAst::Node> nodes_;
    std::size_t pos_ = 0;

    const Token& peek() const { return tokens_[pos_]; }
    bool check(TokenType t) const { return peek().type == t; }

    int emit(ArenaAst::Node n) {
        nodes_.push_back(n);
        return static_cast<int>(nodes_.size()) - 1;
    }

    int expr() {
        int l = term();
        while (check(TokenType::Plus) || check(TokenType::Minus)) {
            const TokenType op = tokens_[pos_++].type;
            const int r = term();
            l = emit({op == TokenType::Plus ? ArenaAst::K::Add : ArenaAst::K::Sub, l, r, 0, 0.0});
        }
        return l;
    }
    int term() {
        int l = unaryRule();
        while (check(TokenType::Star) || check(TokenType::Slash)) {
            const TokenType op = tokens_[pos_++].type;
            const int r = unaryRule();
            l = emit({op == TokenType::Star ? ArenaAst::K::Mul : ArenaAst::K::Div, l, r, 0, 0.0});
        }
        return l;
    }
    int unaryRule() {
        if (check(TokenType::Plus) || check(TokenType::Minus)) {
            const TokenType op = tokens_[pos_++].type;
            const int operand = unaryRule();
            return emit({op == TokenType::Minus ? ArenaAst::K::Neg : ArenaAst::K::Pos,
                         operand, -1, 0, 0.0});
        }
        return power();
    }
    int power() {
        const int base = primary();
        if (check(TokenType::Caret)) {
            ++pos_;
            const int exp = unaryRule();
            return emit({ArenaAst::K::Pow, base, exp, 0, 0.0});
        }
        return base;
    }
    int primary() {
        if (check(TokenType::Number)) {
            return emit({ArenaAst::K::Num, -1, -1, 0, tokens_[pos_++].value});
        }
        if (check(TokenType::Ident)) {
            return emit({ArenaAst::K::Var, -1, -1, static_cast<int>(tokens_[pos_++].value), 0.0});
        }
        if (check(TokenType::LParen)) {
            ++pos_;
            const int e = expr();
            if (!check(TokenType::RParen)) {
                throw std::runtime_error("expected ')' at position " +
                                         std::to_string(peek().pos));
            }
            ++pos_;
            return e;
        }
        throw std::runtime_error("expected number, variable or '(' at position " +
                                 std::to_string(peek().pos));
    }
};

}  // namespace

ArenaAst ArenaAst::parse(std::string_view src) {
    Builder b(src);
    const int root = b.build();
    ArenaAst a;
    a.nodes_ = b.take();
    a.root_ = root;
    return a;
}

double ArenaAst::evalNode(int i, const double* vars) const {
    const Node& nd = nodes_[static_cast<std::size_t>(i)];
    switch (nd.kind) {
        case K::Num: return nd.value;
        case K::Var: return vars[nd.var];
        case K::Pos: return +evalNode(nd.a, vars);
        case K::Neg: return -evalNode(nd.a, vars);
        case K::Add: return evalNode(nd.a, vars) + evalNode(nd.b, vars);
        case K::Sub: return evalNode(nd.a, vars) - evalNode(nd.b, vars);
        case K::Mul: return evalNode(nd.a, vars) * evalNode(nd.b, vars);
        case K::Div: return evalNode(nd.a, vars) / evalNode(nd.b, vars);
        case K::Pow: return std::pow(evalNode(nd.a, vars), evalNode(nd.b, vars));
    }
    throw std::runtime_error("invalid arena node");
}

}  // namespace mp
