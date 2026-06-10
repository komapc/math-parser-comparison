#include "parser/arena_ast.hpp"
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// Reverse (bottom-up) multipass — the dual of multipass_arena.
//
// multipass_arena is TOP-DOWN: find the lowest-precedence operator (the root,
// last to evaluate), split, recurse. This is BOTTOM-UP: reduce the tightest-
// binding things first — deepest parens, then ^, then * /, then + - —
// agglomerating outward until one node remains. It is "multipass" in the
// original sense: one reduction pass per precedence level.
//
// Both produce a bit-identical arena AST; only the construction order differs.
//
// Implementation notes:
//  * All items live in ONE shared vector with stack discipline: each
//    reduceRange works above its own base and truncates back on exit, so a
//    whole eval does no per-range allocation (the buffers warm up once).
//  * A ^/unary segment — (un* opd (^ un* opd)*) — is reduced the moment a
//    * / + - barrier closes it, by a single RIGHT-TO-LEFT walk: right-assoc ^
//    and the ^-binds-tighter-than-unary corner (-2^2 = -4, 2^-3 = 0.125) fall
//    out naturally when folding from the right, with no recursion.
//  * The per-level contractions then rewrite the item stack in place.
//  The only recursion left is per paren group.

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

class MultipassReverse final : public IEvaluator {
    // An item is one of: an operand (a built node), a prefix unary op, or a
    // pending binary op.
    enum class Tag { Opd, Un, Op };
    struct Item {
        Tag       tag;
        int       node;  // Opd: arena index
        TokenType op;    // Un / Op: the operator token
    };

public:
    const char* name() const override { return "multipass-reverse"; }

    double eval(std::string_view src, const double* vars = nullptr) override {
        tokens_ = tokenize(src);
        vars_ = vars;
        nodes_.clear();
        nodes_.reserve(tokens_.size());
        items_.clear();
        buildParenMatch();
        const int root = reduceRange(0, tokens_.size() - 1);
        return evalNode(root);
    }

private:
    std::vector<Token>          tokens_;
    std::vector<ArenaAst::Node> nodes_;
    std::vector<Item>           items_;  // shared item stack (see notes above)
    std::vector<std::size_t>    parenMatch_;
    const double*               vars_ = nullptr;

    int emit(ArenaAst::Node nd) {
        nodes_.push_back(nd);
        return static_cast<int>(nodes_.size()) - 1;
    }

    void buildParenMatch() {
        const std::size_t n = tokens_.size();
        parenMatch_.assign(n, 0);
        std::vector<std::size_t> stack;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            if (tokens_[i].type == TokenType::LParen) {
                stack.push_back(i);
            } else if (tokens_[i].type == TokenType::RParen && !stack.empty()) {
                const std::size_t open = stack.back(); stack.pop_back();
                parenMatch_[open] = i;
                parenMatch_[i]    = open;
            }
        }
    }

    // Reduce items_[segStart..end) — one ^/unary segment — to a single operand
    // by folding right-to-left (right-assoc ^; ^ binds tighter than unary).
    void reduceSegment(std::size_t segStart) {
        std::size_t i = items_.size();
        if (i == segStart) throw std::runtime_error("malformed segment");
        if (i - segStart == 1) {  // common case: a lone operand, nothing to do
            if (items_[segStart].tag != Tag::Opd)
                throw std::runtime_error("malformed segment");
            return;
        }
        --i;
        if (items_[i].tag != Tag::Opd) throw std::runtime_error("malformed segment");
        int acc = items_[i].node;
        while (i > segStart) {
            --i;
            const Item& it = items_[i];
            if (it.tag == Tag::Un) {
                acc = emit({it.op == TokenType::Minus ? ArenaAst::K::Neg
                                                      : ArenaAst::K::Pos,
                            acc, -1, 0, 0.0});
            } else if (it.tag == Tag::Op && it.op == TokenType::Caret &&
                       i > segStart && items_[i - 1].tag == Tag::Opd) {
                --i;
                acc = emit({ArenaAst::K::Pow, items_[i].node, acc, 0, 0.0});
            } else {
                throw std::runtime_error("malformed segment");
            }
        }
        items_.resize(segStart);
        items_.push_back({Tag::Opd, acc, TokenType::End});
    }

    // Left-to-right left-associative contraction of one binary level, in place
    // over items_[base..end) (which alternates Opd (Op Opd)*).
    void reduceBinLevel(std::size_t base, bool mulDiv) {
        std::size_t w = base;  // write index; never overtakes the read index
        const std::size_t end = items_.size();
        for (std::size_t r = base + 1; r + 1 < end; r += 2) {
            const Item op  = items_[r];
            const Item rhs = items_[r + 1];
            const bool inLevel = mulDiv
                ? (op.op == TokenType::Star || op.op == TokenType::Slash)
                : (op.op == TokenType::Plus || op.op == TokenType::Minus);
            if (inLevel) {
                items_[w] = {Tag::Opd,
                             emit({binaryKind(op.op), items_[w].node, rhs.node,
                                   0, 0.0}),
                             TokenType::End};
            } else {
                items_[w + 1] = op;
                items_[w + 2] = rhs;
                w += 2;
            }
        }
        items_.resize(w + 1);
    }

    int reduceRange(std::size_t lo, std::size_t hi) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");
        const std::size_t base = items_.size();

        // 1. materialise items, recursing into parens (innermost first);
        //    a * / + - barrier closes the current ^/unary segment on the fly
        std::size_t segStart = base;
        bool expectOperand = true;
        for (std::size_t i = lo; i < hi; ) {
            const Token& t = tokens_[i];
            switch (t.type) {
                case TokenType::LParen: {
                    const std::size_t j = parenMatch_[i];
                    const int child = reduceRange(i + 1, j);
                    items_.push_back({Tag::Opd, child, TokenType::End});
                    i = j + 1; expectOperand = false; break;
                }
                case TokenType::Number:
                    items_.push_back({Tag::Opd,
                                      emit({ArenaAst::K::Num, -1, -1, 0, t.value}),
                                      TokenType::End});
                    ++i; expectOperand = false; break;
                case TokenType::Ident:
                    items_.push_back({Tag::Opd,
                                      emit({ArenaAst::K::Var, -1, -1,
                                            static_cast<int>(t.value), 0.0}),
                                      TokenType::End});
                    ++i; expectOperand = false; break;
                case TokenType::Plus:
                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Caret:
                    if (expectOperand) {
                        if (t.type != TokenType::Plus && t.type != TokenType::Minus)
                            throw std::runtime_error("unexpected operator");
                        items_.push_back({Tag::Un, -1, t.type}); ++i;
                    } else if (t.type == TokenType::Caret) {
                        items_.push_back({Tag::Op, -1, t.type});
                        ++i; expectOperand = true;
                    } else {  // barrier: close the segment
                        reduceSegment(segStart);
                        items_.push_back({Tag::Op, -1, t.type});
                        segStart = items_.size();
                        ++i; expectOperand = true;
                    }
                    break;
                default:
                    throw std::runtime_error("syntax error");
            }
        }
        if (expectOperand) throw std::runtime_error("unexpected end of sub-expression");
        reduceSegment(segStart);

        // 2. contract * / then + - (skipped when the range was one segment)
        if (items_.size() - base > 1) {
            reduceBinLevel(base, /*mulDiv=*/true);
            reduceBinLevel(base, /*mulDiv=*/false);
        }
        if (items_.size() - base != 1 || items_[base].tag != Tag::Opd)
            throw std::runtime_error("reduction failed");
        const int r = items_[base].node;
        items_.resize(base);
        return r;
    }

    double evalNode(int i) const {
        const ArenaAst::Node& nd = nodes_[static_cast<std::size_t>(i)];
        switch (nd.kind) {
            case ArenaAst::K::Num: return nd.value;
            case ArenaAst::K::Var: return vars_ ? vars_[nd.var] : 0.0;
            case ArenaAst::K::Pos: return +evalNode(nd.a);
            case ArenaAst::K::Neg: return -evalNode(nd.a);
            case ArenaAst::K::Add: return evalNode(nd.a) + evalNode(nd.b);
            case ArenaAst::K::Sub: return evalNode(nd.a) - evalNode(nd.b);
            case ArenaAst::K::Mul: return evalNode(nd.a) * evalNode(nd.b);
            case ArenaAst::K::Div: return evalNode(nd.a) / evalNode(nd.b);
            case ArenaAst::K::Pow: return std::pow(evalNode(nd.a), evalNode(nd.b));
        }
        throw std::runtime_error("invalid node");
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_multipass_reverse() {
    return std::make_unique<MultipassReverse>();
}

}  // namespace mp
