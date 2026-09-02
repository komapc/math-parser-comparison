#include "parser/arena_ast.hpp"
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// multipass-reverse, fused: the same bottom-up order (deepest parens, then
// ^/unary, then * /, then + -), with the two binary levels contracted in the
// SAME left-to-right sweep that materialises the items.
//
// After a ^/unary segment folds on a barrier, all that is left of the grammar
// is two left-associative levels — sum := term ((+|-) term)*,
// term := seg ((*|/) seg)* — and a bottom-up reducer for that needs exactly
// two accumulators, not a list: `term` collects the current * / run, `sum`
// collects the + - run, each with its pending operator. A + or - barrier
// closes both the segment and the term; a * or / barrier closes the segment
// only. Parentheses push those accumulators on a frame stack, so there is no
// recursion, no paren-matching prepass and no per-level re-scan of an item
// list: every token is touched once, in order, and the ^/unary segment buffer
// is the only working memory (needed because ^ is right-associative and binds
// tighter than the prefix sign, so a segment folds right-to-left).
//
// Still "multipass" in the precedence-level sense — each level is reduced
// bottom-up, tightest first, and nothing ever looks for a split point — so its
// worst case is still its average case. It just performs the three level
// passes of multipass_reverse.cpp on the fly instead of over a buffer.

// Operand type V is an arena node index (tree variant) or a double (direct).
// Policy supplies leaf/unary/binary construction.
//
// Register discipline: the operand most recently completed lives in `val`,
// not in memory. The segment buffer only receives the parts of a ^/unary
// segment that are still pending — a prefix sign, or an operand that turned
// out to be the base of a ^ — so on a plain "a * b + c" corpus the buffer is
// never written at all: the whole reduction runs in val/term/sum. Both stacks
// are pre-sized raw buffers indexed by locals (not std::vector push/pop):
// a barrier then costs a compare, not a size() load — measured ~10 % on the
// random corpus, the difference between trailing and tying recursive descent.
//
// Tokens come from the shared lexer in streaming mode: the sweep reads left
// to right with one token of lookahead, so no token array is ever built —
// the same treatment every other left-to-right strategy gets (see lexer.hpp).
template <class Policy>
class ReverseFold final : public IEvaluator {
    using V = typename Policy::V;
    enum class Tag : unsigned char { Un, PowBase };
    struct Item { V v; Tag tag; TokenType op; };
    struct Frame {
        V             term, sum;
        std::uint32_t segBase;
        TokenType     pendMul, pendAdd;   // End = nothing pending
    };

public:
    const char* name() const override { return Policy::kName; }

    double eval(std::string_view src, const double* vars = nullptr) override {
        // Each token pushes at most one item / one frame, and there is at most
        // one token per source byte, so the source length bounds both stacks;
        // the buffers persist across evals (warm).
        const std::size_t bound = src.size() + 1;
        pol_.begin(bound, vars);
        if (seg_.size() < bound) {
            seg_.resize(bound);
            frames_.resize(bound);
        }
        Item*  const  seg    = seg_.data();
        Frame* const  frames = frames_.data();
        std::uint32_t segTop = 0, segBase = 0, nFrames = 0;

        // The live frame is kept as scalars (never address-taken) so the
        // compiler can hold it in registers; a Frame is only materialised
        // when a '(' pushes it.
        V             term{}, sum{}, val{};
        TokenType     pendMul = TokenType::End, pendAdd = TokenType::End;

        // One token of lookahead over the stream: `cur` is the next token.
        // End repeats once the input is exhausted.
        Lexer lx(src);
        Token cur = lx.next();
        const auto advance = [&]() -> Token {
            const Token t = cur;
            cur = lx.next();
            return t;
        };

        // Fold the segment ending in `c` right-to-left (right-assoc ^; ^ binds
        // tighter than a prefix sign). The state machine guarantees the buffer
        // above segBase is a well-formed (Un | PowBase)* prefix, so no re-checks.
        const auto foldSegment = [&](V c) -> V {
            if (segTop == segBase) return c;  // lone operand: no memory touched
            V acc = c;
            std::uint32_t i = segTop;
            do {
                --i;
                const Item& it = seg[i];
                acc = (it.tag == Tag::Un) ? pol_.unary(it.op, acc) : pol_.pow(it.v, acc);
            } while (i > segBase);
            segTop = segBase;
            return acc;
        };
        // End of a range (')' or End): flush segment -> term -> sum.
        const auto closeRange = [&](V c) -> V {
            const V v = foldSegment(c);
            const V t = (pendMul == TokenType::End) ? v : pol_.mulDiv(pendMul, term, v);
            return (pendAdd == TokenType::End) ? t : pol_.addSub(pendAdd, sum, t);
        };

        // Two-mode scan. The grammar alternates operand / operator, so each
        // mode dispatches over its own small set of token kinds — the same
        // context recursive descent gets from its call structure, without the
        // calls. Tests are ordered by corpus frequency. Malformed input is
        // whatever falls out of either mode.
        for (;;) {
            // -- operand mode: number, variable, '(' or a prefix sign
            for (;;) {
                const Token t = advance();
                if (t.type == TokenType::Number) { val = pol_.num(t.value); break; }
                if (t.type == TokenType::LParen) {
                    frames[nFrames++] = {term, sum, segBase, pendMul, pendAdd};
                    segBase = segTop;
                    pendMul = pendAdd = TokenType::End;
                    continue;
                }
                if (t.type == TokenType::Minus || t.type == TokenType::Plus) {
                    // Fast path: a sign on a plain leaf that is not the base of
                    // a ^ ("-16", "-a") folds straight into the register. Take
                    // the leaf, then look at the one token after it: if it is
                    // not ^ the sign applies now; if it is, the sign stays
                    // pending in the segment and the leaf is the base.
                    if (cur.type == TokenType::Number || cur.type == TokenType::Ident) {
                        const Token leaf = advance();
                        const V lv = (leaf.type == TokenType::Number)
                                         ? pol_.num(leaf.value)
                                         : pol_.var(static_cast<int>(leaf.value));
                        if (cur.type != TokenType::Caret) { val = pol_.unary(t.type, lv); break; }
                        seg[segTop++] = {V{}, Tag::Un, t.type};
                        val = lv; break;
                    }
                    seg[segTop++] = {V{}, Tag::Un, t.type};
                    continue;
                }
                if (t.type == TokenType::Ident) { val = pol_.var(static_cast<int>(t.value)); break; }
                throw std::runtime_error("expected operand at position " + std::to_string(t.pos));
            }
            // -- operator mode: binary op, ')' or end; ')' stays in this mode
            for (;;) {
                const Token t = advance();
                if (t.type == TokenType::Plus || t.type == TokenType::Minus) {
                    // + - barrier: closes the segment, the term and the sum
                    const V v = foldSegment(val);
                    term = (pendMul == TokenType::End) ? v : pol_.mulDiv(pendMul, term, v);
                    sum  = (pendAdd == TokenType::End) ? term : pol_.addSub(pendAdd, sum, term);
                    pendAdd = t.type;
                    pendMul = TokenType::End;
                    break;
                }
                if (t.type == TokenType::RParen) {
                    if (nFrames == 0)
                        throw std::runtime_error("mismatched parenthesis at position " +
                                                 std::to_string(t.pos));
                    val = closeRange(val);
                    const Frame& f = frames[--nFrames];
                    term = f.term; sum = f.sum; segBase = f.segBase;
                    pendMul = f.pendMul; pendAdd = f.pendAdd;
                    continue;
                }
                if (t.type == TokenType::Star || t.type == TokenType::Slash) {
                    // * / barrier: closes the segment into the term
                    const V v = foldSegment(val);
                    term = (pendMul == TokenType::End) ? v : pol_.mulDiv(pendMul, term, v);
                    pendMul = t.type;
                    break;
                }
                if (t.type == TokenType::Caret) {
                    seg[segTop++] = {val, Tag::PowBase, t.type};
                    break;
                }
                if (t.type == TokenType::End) {
                    if (nFrames != 0) throw std::runtime_error("missing ')'");
                    return pol_.finish(closeRange(val));
                }
                throw std::runtime_error("expected operator at position " + std::to_string(t.pos));
            }
        }
    }

private:
    std::vector<Item>  seg_;      // pending ^/unary parts (shared across frames)
    std::vector<Frame> frames_;   // one per open parenthesis
    Policy             pol_;
};

// ---- tree policy: build an arena AST, then walk it ------------------------
struct TreePolicy {
    using V = int;
    static constexpr const char* kName = "multipass-reverse-fold";
    std::vector<ArenaAst::Node> nodes;
    const double* vars = nullptr;

    void begin(std::size_t bound, const double* v) {
        nodes.clear(); nodes.reserve(bound); vars = v;
    }
    int emit(ArenaAst::Node nd) { nodes.push_back(nd); return static_cast<int>(nodes.size()) - 1; }
    int num(double x)  { return emit({ArenaAst::K::Num, -1, -1, 0, x}); }
    int var(int idx)   { return emit({ArenaAst::K::Var, -1, -1, idx, 0.0}); }
    int unary(TokenType op, int a) {
        return emit({op == TokenType::Minus ? ArenaAst::K::Neg : ArenaAst::K::Pos, a, -1, 0, 0.0});
    }
    int mulDiv(TokenType op, int a, int b) {
        return emit({op == TokenType::Star ? ArenaAst::K::Mul : ArenaAst::K::Div, a, b, 0, 0.0});
    }
    int addSub(TokenType op, int a, int b) {
        return emit({op == TokenType::Plus ? ArenaAst::K::Add : ArenaAst::K::Sub, a, b, 0, 0.0});
    }
    int pow(int a, int b) { return emit({ArenaAst::K::Pow, a, b, 0, 0.0}); }
    double finish(int root) const { return evalNode(root); }

    double evalNode(int i) const {
        const ArenaAst::Node& nd = nodes[static_cast<std::size_t>(i)];
        switch (nd.kind) {
            case ArenaAst::K::Num: return nd.value;
            case ArenaAst::K::Var: return vars ? vars[nd.var] : 0.0;
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

// ---- direct policy: operands are values, nothing is built ----------------
struct DirectPolicy {
    using V = double;
    static constexpr const char* kName = "direct-reverse";
    const double* vars = nullptr;

    void begin(std::size_t, const double* v) { vars = v; }
    double num(double x) const { return x; }
    double var(int idx) const  { return vars ? vars[idx] : 0.0; }
    double unary(TokenType op, double a) const { return op == TokenType::Minus ? -a : a; }
    double mulDiv(TokenType op, double a, double b) const { return op == TokenType::Star ? a * b : a / b; }
    double addSub(TokenType op, double a, double b) const { return op == TokenType::Plus ? a + b : a - b; }
    double pow(double a, double b) const { return std::pow(a, b); }
    double finish(double v) const { return v; }
};

}  // namespace

std::unique_ptr<IEvaluator> make_multipass_reverse_fold() {
    return std::make_unique<ReverseFold<TreePolicy>>();
}
std::unique_ptr<IEvaluator> make_direct_reverse() {
    return std::make_unique<ReverseFold<DirectPolicy>>();
}

}  // namespace mp
