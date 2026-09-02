#include "parser/arena_ast.hpp"

#include "parser/lexer.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace mp {
namespace {

// In one sentence: ordinary recursive descent, but every AST node is appended to
// one contiguous vector and children are referenced by index instead of pointer.
//
// Recursive-descent builder that emits nodes into a flat vector and returns the
// index of each subtree's root. Same grammar as src/recursive_descent.cpp.
class Builder {
public:
    // Emits into the caller's node vector so its capacity survives across
    // parses (see ArenaAst::reparse).
    Builder(std::string_view src, std::vector<ArenaAst::Node>& nodes)
        : lx_(src), nodes_(nodes) {
        cur_ = lx_.next();
        nodes_.clear();
        // A node per token at most; ~one token per two source bytes on the
        // spaced corpora (a dense input may grow the buffer once, after
        // which its capacity persists across reparses).
        nodes_.reserve(src.size() / 2 + 1);
    }

    int build() {
        const int root = expr();
        if (!check(TokenType::End)) {
            throw std::runtime_error("unexpected token at position " +
                                     std::to_string(peek().pos));
        }
        return root;
    }

private:
    // Streaming lexer + one token of lookahead; no token array.
    Lexer lx_;
    Token cur_;
    std::vector<ArenaAst::Node>& nodes_;

    const Token& peek() const { return cur_; }
    Token take() { const Token t = cur_; cur_ = lx_.next(); return t; }
    bool check(TokenType t) const { return peek().type == t; }

    int emit(ArenaAst::Node n) {
        nodes_.push_back(n);
        return static_cast<int>(nodes_.size()) - 1;
    }

    int expr() {
        int l = term();
        while (check(TokenType::Plus) || check(TokenType::Minus)) {
            const TokenType op = take().type;
            const int r = term();
            l = emit({op == TokenType::Plus ? ArenaAst::K::Add : ArenaAst::K::Sub, l, r, 0, 0.0});
        }
        return l;
    }
    int term() {
        int l = unaryRule();
        while (check(TokenType::Star) || check(TokenType::Slash)) {
            const TokenType op = take().type;
            const int r = unaryRule();
            l = emit({op == TokenType::Star ? ArenaAst::K::Mul : ArenaAst::K::Div, l, r, 0, 0.0});
        }
        return l;
    }
    int unaryRule() {
        if (check(TokenType::Plus) || check(TokenType::Minus)) {
            const TokenType op = take().type;
            const int operand = unaryRule();
            // materialise Pos like every other strategy, so all builders
            // produce structurally identical trees
            return emit({op == TokenType::Plus ? ArenaAst::K::Pos : ArenaAst::K::Neg,
                         operand, -1, 0, 0.0});
        }
        return power();
    }
    int power() {
        const int base = primary();
        if (check(TokenType::Caret)) {
            take();
            const int exp = unaryRule();
            return emit({ArenaAst::K::Pow, base, exp, 0, 0.0});
        }
        return base;
    }
    int primary() {
        if (check(TokenType::Number)) {
            return emit({ArenaAst::K::Num, -1, -1, 0, take().value});
        }
        if (check(TokenType::Ident)) {
            return emit({ArenaAst::K::Var, -1, -1, static_cast<int>(take().value), 0.0});
        }
        if (check(TokenType::LParen)) {
            take();
            const int e = expr();
            if (!check(TokenType::RParen)) {
                throw std::runtime_error("expected ')' at position " +
                                         std::to_string(peek().pos));
            }
            take();
            return e;
        }
        throw std::runtime_error("expected number, variable or '(' at position " +
                                 std::to_string(peek().pos));
    }
};

}  // namespace

ArenaAst ArenaAst::parse(std::string_view src) {
    ArenaAst a;
    a.reparse(src);
    return a;
}

void ArenaAst::reparse(std::string_view src) {
    root_ = -1;  // a throwing build must not leave root_ into the cleared buffer
    Builder b(src, nodes_);
    root_ = b.build();
}

ArenaAst ArenaAst::adopt(std::vector<Node> nodes, int root) {
    ArenaAst a;
    a.nodes_ = std::move(nodes);
    a.root_ = root;
    return a;
}

double ArenaAst::evalNode(int i, const double* vars) const {
    const Node& nd = nodes_[static_cast<std::size_t>(i)];
    switch (nd.kind) {
        case K::Num: return nd.value;
        // null vars are substituted with a zero table at the evaluator entry
        // points (see evaluators.cpp), keeping this hot path branch-free
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
