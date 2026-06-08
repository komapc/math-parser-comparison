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

// Arena divide-and-conquer parser — same algorithm as multipass.cpp but with:
//
//   Arena AST       — one contiguous vector, no per-node heap allocation.
//   parenMatch[]    — O(1) paren-strip instead of O(range) scan.
//   Iterator passing — the critical new optimization explained below.
//
// ── ITERATOR PASSING ────────────────────────────────────────────────────────
// Every parseRange() call used to do a O(log n) binary search to locate its
// candidate sub-array.  Profiling showed ~1.70 binary searches per call
// (splits + flat-chain folds + paren strips) across all expression sizes.
//
// The key insight: when we split at position k, the two sub-ranges' candidate
// spans are ALREADY KNOWN — they're just the left and right halves of [cbeg,
// cend).  So we pass the iterator pair DOWN instead of re-deriving it:
//
//   split at k  →  left  gets [cbeg, split_it)    O(1)
//                  right gets [split_it+1, cend)  O(1)
//
// Flat-chain sub-ranges between chain operators have ZERO depth-d candidates
// by definition (otherwise they'd be in the chain), so they get (cend,cend). O(1)
//
// The only call that still needs a binary search is a PAREN STRIP, which
// changes the nesting depth from d to d+1 and must look up candsByDepth_[d+1].
// Everything else becomes O(1) per call.
//
// Result: binary searches drop from ~24k to ~3.3k per 10000-leaf expression
// (a 7× reduction). Bracket-free expressions drop to 1 binary search total.
//
// ── COMPLEXITY ──────────────────────────────────────────────────────────────
// The scan inside findSplit is still O(k) per call (candidates in range).
// With the flat-chain fold eliminating same-precedence levels and iterator
// passing eliminating binary-search overhead, the dominant remaining cost is
// the inherent O(n log n) scan work of the divide-and-conquer structure.
// That is irreducible without a sparse table / RMQ structure.

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

struct Candidate {
    std::size_t pos;
    int         prec;
    bool        rightAssoc;
    bool        unary;
};

class MultiPassArena final : public IEvaluator {
    using CIt = std::vector<Candidate>::const_iterator;

public:
    const char* name() const override { return "multipass-arena"; }

    ArenaAst buildArena(std::string_view src) {
        tokens_ = tokenize(src);
        nodes_.clear();
        nodes_.reserve(tokens_.size());
        buildCandidates();
        const std::size_t n = tokens_.size() - 1;
        auto [cbeg, cend] = candRange(0, n, 0);
        const int root = parseRange(0, n, 0, cbeg, cend);
        return ArenaAst::adopt(std::move(nodes_), root);
    }

    double eval(std::string_view src) override {
        return buildArena(src).eval(nullptr);
    }

private:
    std::vector<Token>                  tokens_;
    std::vector<ArenaAst::Node>         nodes_;
    std::vector<std::vector<Candidate>> candsByDepth_;
    std::vector<std::size_t>            parenMatch_;

    // One O(n) pass: build per-depth candidate lists + precomputed paren matches.
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
                        const std::size_t open = stack.back(); stack.pop_back();
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

    // O(log n) — only called on paren strips (depth change) and the initial call.
    std::pair<CIt, CIt> candRange(std::size_t lo, std::size_t hi, int depth) const {
        static const std::vector<Candidate> empty;
        const auto& v = (depth >= 0 && depth < static_cast<int>(candsByDepth_.size()))
                        ? candsByDepth_[static_cast<std::size_t>(depth)] : empty;
        auto b = std::lower_bound(v.begin(), v.end(), lo, ByPos{});
        auto e = std::lower_bound(b,         v.end(), hi, ByPos{});
        return {b, e};
    }

    // Returns iterator to the split candidate, or `end` if none.
    // RTL scan: left-assoc tie-break (rightmost) = free; early exit at prec==1.
    CIt findSplit(CIt beg, CIt end, std::size_t lo) const {
        if (beg == end) return end;
        CIt best = end;
        for (auto it = end; it != beg; ) {
            --it;
            if (it->unary && it->pos != lo) continue;
            if (best == end || it->prec < best->prec) {
                best = it;
                if (best->prec == 1 && !best->rightAssoc) break;
            } else if (it->prec == best->prec && best->rightAssoc) {
                best = it;  // right-assoc: prefer leftmost
            }
        }
        return best;
    }

    int emit(ArenaAst::Node nd) {
        nodes_.push_back(nd);
        return static_cast<int>(nodes_.size()) - 1;
    }

    // cbeg/cend are the already-located candidates at `depth` in [lo, hi).
    // For splits: sub-ranges receive sliced iterators — NO binary search.
    // For paren strips: binary search into candsByDepth_[depth+1] — unavoidable.
    int parseRange(std::size_t lo, std::size_t hi, int depth, CIt cbeg, CIt cend) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        // Flat-chain: all candidates share one precedence — fold iteratively.
        // Sub-ranges between operators have zero depth-d candidates, so they
        // receive empty iterator pairs (O(1), no binary search).
        if (cbeg != cend) {
            const int  chainPrec = cbeg->prec;
            const bool chainRa   = cbeg->rightAssoc;
            bool flat = true;
            for (auto it = cbeg; it != cend; ++it)
                if (it->unary || it->prec != chainPrec) { flat = false; break; }

            if (flat) {
                if (!chainRa) {
                    // Left fold: ((a op b) op c) op d …
                    int acc = parseRange(lo, cbeg->pos, depth, cend, cend);
                    for (auto it = cbeg; it != cend; ++it) {
                        const std::size_t nextLo = it->pos + 1;
                        const std::size_t nextHi = (it + 1 != cend) ? (it+1)->pos : hi;
                        int rhs = parseRange(nextLo, nextHi, depth, cend, cend);
                        acc = emit({binaryKind(tokens_[it->pos].type), acc, rhs, 0, 0.0});
                    }
                    return acc;
                } else {
                    // Right fold: a op (b op (c op d)) …
                    std::vector<int> parts;
                    parts.reserve(static_cast<std::size_t>(cend - cbeg) + 1);
                    std::size_t prevLo = lo;
                    for (auto it = cbeg; it != cend; ++it) {
                        parts.push_back(parseRange(prevLo, it->pos, depth, cend, cend));
                        prevLo = it->pos + 1;
                    }
                    parts.push_back(parseRange(prevLo, hi, depth, cend, cend));
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

        // General split: pass sliced iterators to sub-ranges — O(1), no search.
        const CIt splitIt = findSplit(cbeg, cend, lo);
        if (splitIt != cend) {
            if (splitIt->unary) {
                int operand = parseRange(splitIt->pos + 1, hi, depth,
                                         splitIt + 1, cend);
                const ArenaAst::K k = (tokens_[splitIt->pos].type == TokenType::Minus)
                                      ? ArenaAst::K::Neg : ArenaAst::K::Pos;
                return emit({k, operand, -1, 0, 0.0});
            }
            int lhs = parseRange(lo,               splitIt->pos, depth, cbeg,        splitIt);
            int rhs = parseRange(splitIt->pos + 1, hi,           depth, splitIt + 1, cend);
            return emit({binaryKind(tokens_[splitIt->pos].type), lhs, rhs, 0, 0.0});
        }

        // No depth-d operator: paren group or leaf.
        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi - 1) {
            // Depth increases by 1 — one binary search to locate the new candidates.
            auto [b2, e2] = candRange(lo + 1, hi - 1, depth + 1);
            return parseRange(lo + 1, hi - 1, depth + 1, b2, e2);
        }
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
            case ArenaAst::K::Var: return 0.0;
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

std::unique_ptr<IEvaluator> make_ast_multipass_arena() {
    return std::make_unique<MultiPassArena>();
}

ArenaAst multipass_arena_parse(std::string_view src) {
    MultiPassArena p;
    return p.buildArena(src);
}

}  // namespace mp
