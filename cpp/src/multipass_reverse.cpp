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
// reduceSegment recurses on the unary/power grammar (right-assoc ^, prefix
// unary) so the ^-binds-tighter-than-unary corner (-2^2 = -4, 2^-3 = 0.125)
// stays correct.

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
        buildParenMatch();
        const int root = reduceRange(0, tokens_.size() - 1);
        return evalNode(root);
    }

private:
    std::vector<Token>          tokens_;
    std::vector<ArenaAst::Node> nodes_;
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

    static bool isBarrier(TokenType t) {
        return t == TokenType::Plus || t == TokenType::Minus ||
               t == TokenType::Star || t == TokenType::Slash;
    }

    // Reduce one ^/unary segment (operands, prefix unary, ^) to a single node.
    int reduceSegment(const std::vector<Item>& seg) {
        std::size_t pos = 0;
        const int r = segUnary(seg, pos);
        if (pos != seg.size()) throw std::runtime_error("malformed segment");
        return r;
    }
    int segOperand(const std::vector<Item>& seg, std::size_t& pos) {
        if (pos >= seg.size() || seg[pos].tag != Tag::Opd)
            throw std::runtime_error("expected operand");
        return seg[pos++].node;
    }
    int segPower(const std::vector<Item>& seg, std::size_t& pos) {
        const int base = segOperand(seg, pos);
        if (pos < seg.size() && seg[pos].tag == Tag::Op &&
            seg[pos].op == TokenType::Caret) {
            ++pos;
            const int rhs = segUnary(seg, pos);  // right-assoc, exponent is a unary
            return emit({ArenaAst::K::Pow, base, rhs, 0, 0.0});
        }
        return base;
    }
    int segUnary(const std::vector<Item>& seg, std::size_t& pos) {
        if (pos < seg.size() && seg[pos].tag == Tag::Un) {
            const TokenType op = seg[pos].op; ++pos;
            const int c = segUnary(seg, pos);
            return emit({op == TokenType::Minus ? ArenaAst::K::Neg : ArenaAst::K::Pos,
                         c, -1, 0, 0.0});
        }
        return segPower(seg, pos);
    }

    // Left-to-right left-associative contraction of one binary level.
    void reduceBinLevel(std::vector<Item>& flat, bool mulDiv) {
        std::vector<Item> out;
        out.reserve(flat.size());
        out.push_back(flat[0]);
        for (std::size_t i = 1; i + 1 < flat.size(); i += 2) {
            const Item op = flat[i];
            const Item rhs = flat[i + 1];
            const bool inLevel = mulDiv
                ? (op.op == TokenType::Star || op.op == TokenType::Slash)
                : (op.op == TokenType::Plus || op.op == TokenType::Minus);
            if (inLevel) {
                const Item left = out.back(); out.pop_back();
                out.push_back({Tag::Opd,
                               emit({binaryKind(op.op), left.node, rhs.node, 0, 0.0}),
                               TokenType::End});
            } else {
                out.push_back(op);
                out.push_back(rhs);
            }
        }
        flat = std::move(out);
    }

    int reduceRange(std::size_t lo, std::size_t hi) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        // 1. materialise items, recursing into parens (innermost first)
        std::vector<Item> raw;
        bool expectOperand = true;
        for (std::size_t i = lo; i < hi; ) {
            const Token& t = tokens_[i];
            switch (t.type) {
                case TokenType::LParen: {
                    const std::size_t j = parenMatch_[i];
                    raw.push_back({Tag::Opd, reduceRange(i + 1, j), TokenType::End});
                    i = j + 1; expectOperand = false; break;
                }
                case TokenType::Number:
                    raw.push_back({Tag::Opd, emit({ArenaAst::K::Num, -1, -1, 0, t.value}),
                                   TokenType::End});
                    ++i; expectOperand = false; break;
                case TokenType::Ident:
                    raw.push_back({Tag::Opd,
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
                        raw.push_back({Tag::Un, -1, t.type}); ++i;
                    } else {
                        raw.push_back({Tag::Op, -1, t.type}); ++i; expectOperand = true;
                    }
                    break;
                default:
                    throw std::runtime_error("syntax error");
            }
        }
        if (expectOperand) throw std::runtime_error("unexpected end of sub-expression");

        // 2. split by * / + - barriers; reduce each ^/unary segment to one operand
        std::vector<Item> flat;
        std::vector<Item> seg;
        for (const Item& it : raw) {
            if (it.tag == Tag::Op && isBarrier(it.op)) {
                flat.push_back({Tag::Opd, reduceSegment(seg), TokenType::End});
                seg.clear();
                flat.push_back(it);
            } else {
                seg.push_back(it);
            }
        }
        flat.push_back({Tag::Opd, reduceSegment(seg), TokenType::End});

        // 3. contract * / then + -
        reduceBinLevel(flat, /*mulDiv=*/true);
        reduceBinLevel(flat, /*mulDiv=*/false);
        if (flat.size() != 1 || flat[0].tag != Tag::Opd)
            throw std::runtime_error("reduction failed");
        return flat[0].node;
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
