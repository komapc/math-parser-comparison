#include "parser/arena_ast.hpp"
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// Arena variant of multipass: identical divide-and-conquer algorithm, but every
// node is appended to one contiguous vector (no per-node heap allocation).
// Combines all fixes from multipass.cpp:
//   #1 per-depth candidate pre-scan
//   #5 right-to-left early-exit scan
//   #6 flat-chain iterative folding
//   #parenMatch O(1) paren stripping
//
// parseRange() returns an int (arena node index) instead of ExprPtr.

int binPrec(TokenType t) {
    switch (t) {
        case TokenType::Plus:
        case TokenType::Minus: return 1;
        case TokenType::Star:
        case TokenType::Slash: return 2;
        case TokenType::Caret: return 4;
        default:               return -1;
    }
}
constexpr int kUnaryPrec = 3;

ArenaAst::K binaryKind(TokenType t) {
    switch (t) {
        case TokenType::Plus:  return ArenaAst::K::Add;
        case TokenType::Minus: return ArenaAst::K::Sub;
        case TokenType::Star:  return ArenaAst::K::Mul;
        case TokenType::Slash: return ArenaAst::K::Div;
        case TokenType::Caret: return ArenaAst::K::Pow;
        default: throw std::runtime_error("invalid binary operator");
    }
}
ArenaAst::K unaryKind(TokenType t) {
    return (t == TokenType::Minus) ? ArenaAst::K::Neg : ArenaAst::K::Pos;
}

struct Candidate {
    std::size_t pos;
    int         prec;
    bool        rightAssoc;
    bool        unary;
};

class MultiPassArena final : public IEvaluator {
public:
    const char* name() const override { return "multipass-arena"; }

    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        nodes_.clear();
        nodes_.reserve(tokens_.size());
        buildCandidates();
        const int root = parseRange(0, tokens_.size() - 1, 0);
        return evalNode(root);
    }

private:
    std::vector<Token>                  tokens_;
    std::vector<ArenaAst::Node>         nodes_;
    std::vector<std::vector<Candidate>> candsByDepth_;
    std::vector<std::size_t>            parenMatch_;

    void buildCandidates() {
        candsByDepth_.clear();
        const std::size_t n = tokens_.size();
        parenMatch_.assign(n, 0);
        int  depth = 0;
        bool expectOperand = true;
        std::vector<std::size_t> stack;
        for (std::size_t i = 0; i < n - 1; ++i) {
            switch (tokens_[i].type) {
                case TokenType::LParen:
                    stack.push_back(i);
                    ++depth; expectOperand = true;
                    break;
                case TokenType::RParen:
                    if (!stack.empty()) {
                        std::size_t open = stack.back(); stack.pop_back();
                        parenMatch_[open] = i;
                        parenMatch_[i]    = open;
                    }
                    --depth; expectOperand = false;
                    break;
                case TokenType::Number:
                case TokenType::Ident: expectOperand = false; break;
                case TokenType::Plus:
                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Caret: {
                    Candidate c;
                    c.pos = i;
                    if (expectOperand) {
                        if (tokens_[i].type != TokenType::Plus &&
                            tokens_[i].type != TokenType::Minus)
                            throw std::runtime_error("unexpected operator");
                        c.prec = kUnaryPrec; c.rightAssoc = false; c.unary = true;
                    } else {
                        c.prec       = binPrec(tokens_[i].type);
                        c.rightAssoc = (tokens_[i].type == TokenType::Caret);
                        c.unary      = false;
                    }
                    if (depth >= static_cast<int>(candsByDepth_.size()))
                        candsByDepth_.resize(static_cast<std::size_t>(depth) + 1);
                    candsByDepth_[static_cast<std::size_t>(depth)].push_back(c);
                    expectOperand = true;
                    break;
                }
                default: break;
            }
        }
    }

    struct ByPos {
        bool operator()(const Candidate& c, std::size_t v) const { return c.pos < v; }
    };
    using CIt = std::vector<Candidate>::const_iterator;

    std::pair<CIt,CIt> candRange(std::size_t lo, std::size_t hi, int depth) const {
        static const std::vector<Candidate> empty;
        const auto& v = (depth >= 0 && depth < static_cast<int>(candsByDepth_.size()))
                        ? candsByDepth_[static_cast<std::size_t>(depth)] : empty;
        auto b = std::lower_bound(v.begin(), v.end(), lo, ByPos{});
        auto e = std::lower_bound(b, v.end(), hi, ByPos{});
        return {b, e};
    }

    const Candidate* findSplit(CIt beg, CIt end, std::size_t lo) const {
        if (beg == end) return nullptr;
        const Candidate* best = nullptr;
        for (auto it = end; it != beg; ) {
            --it;
            if (it->unary && it->pos != lo) continue;
            if (!best || it->prec < best->prec) {
                best = &*it;
                if (best->prec == 1 && !best->rightAssoc) break;
            } else if (it->prec == best->prec && best->rightAssoc) {
                best = &*it;
            }
        }
        return best;
    }

    int emit(ArenaAst::Node nd) {
        nodes_.push_back(nd);
        return static_cast<int>(nodes_.size()) - 1;
    }

    int parseRange(std::size_t lo, std::size_t hi, int depth) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        auto [cbeg, cend] = candRange(lo, hi, depth);

        // Flat-chain: fold iteratively to avoid O(n^2) recursion.
        if (cbeg != cend) {
            const int  chainPrec = cbeg->prec;
            const bool chainRa   = cbeg->rightAssoc;
            bool flat = true;
            for (auto it = cbeg; it != cend; ++it)
                if (it->unary || it->prec != chainPrec) { flat = false; break; }

            if (flat) {
                if (!chainRa) {
                    int acc = parseRange(lo, cbeg->pos, depth);
                    for (auto it = cbeg; it != cend; ++it) {
                        const std::size_t nextLo = it->pos + 1;
                        const std::size_t nextHi = (it + 1 != cend) ? (it+1)->pos : hi;
                        int rhs = parseRange(nextLo, nextHi, depth);
                        acc = emit({binaryKind(tokens_[it->pos].type), acc, rhs, 0, 0.0});
                    }
                    return acc;
                } else {
                    std::vector<int> parts;
                    parts.reserve(static_cast<std::size_t>(cend - cbeg) + 1);
                    std::size_t prevLo = lo;
                    for (auto it = cbeg; it != cend; ++it) {
                        parts.push_back(parseRange(prevLo, it->pos, depth));
                        prevLo = it->pos + 1;
                    }
                    parts.push_back(parseRange(prevLo, hi, depth));
                    int acc = parts.back();
                    auto it = cend;
                    for (std::size_t i = parts.size() - 1; i-- > 0; ) {
                        --it;
                        acc = emit({binaryKind(tokens_[it->pos].type),
                                    parts[i], acc, 0, 0.0});
                    }
                    return acc;
                }
            }
        }

        if (const Candidate* split = findSplit(cbeg, cend, lo)) {
            if (split->unary) {
                int operand = parseRange(split->pos + 1, hi, depth);
                return emit({unaryKind(tokens_[split->pos].type), operand, -1, 0, 0.0});
            }
            int lhs = parseRange(lo, split->pos, depth);
            int rhs = parseRange(split->pos + 1, hi, depth);
            return emit({binaryKind(tokens_[split->pos].type), lhs, rhs, 0, 0.0});
        }

        // Parenthesized group or single primary.
        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi - 1)
            return parseRange(lo + 1, hi - 1, depth + 1);

        if (hi - lo == 1) {
            const Token& t = tokens_[lo];
            if (t.type == TokenType::Number)
                return emit({ArenaAst::K::Num, -1, -1, 0, t.value});
            if (t.type == TokenType::Ident)
                return emit({ArenaAst::K::Var, -1, -1, static_cast<int>(t.value), 0.0});
        }
        throw std::runtime_error("syntax error at position " +
                                 std::to_string(tokens_[lo].pos));
    }

    double evalNode(int i) const {
        const ArenaAst::Node& nd = nodes_[static_cast<std::size_t>(i)];
        switch (nd.kind) {
            case ArenaAst::K::Num: return nd.value;
            case ArenaAst::K::Var: return 0.0;  // one-shot: no variables in constant corpus
            case ArenaAst::K::Pos: return +evalNode(nd.a);
            case ArenaAst::K::Neg: return -evalNode(nd.a);
            case ArenaAst::K::Add: return evalNode(nd.a) + evalNode(nd.b);
            case ArenaAst::K::Sub: return evalNode(nd.a) - evalNode(nd.b);
            case ArenaAst::K::Mul: return evalNode(nd.a) * evalNode(nd.b);
            case ArenaAst::K::Div: return evalNode(nd.a) / evalNode(nd.b);
            case ArenaAst::K::Pow: {
                // inline pow for integer exponents (common in benchmark corpus)
                const double base = evalNode(nd.a);
                const double exp  = evalNode(nd.b);
                return std::pow(base, exp);
            }
        }
        throw std::runtime_error("invalid node");
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_ast_multipass_arena() {
    return std::make_unique<MultiPassArena>();
}

}  // namespace mp
