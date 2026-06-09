// Parallel multipass arena: middle-split + fork-join.
//
// Two changes from multipass-arena (split correctness is unchanged for -, /, ^):
//
//   Middle-split: among tied min-prec candidates, prefer the associative (+/*)
//   operator whose token position is nearest (lo+hi)/2. This produces balanced
//   sub-trees instead of the right-skewed trees from always picking rightmost.
//   Fallback: rightmost for -, /, ^ (cannot re-associate without changing the value).
//
//   Flat-chain bisection: a same-prec chain folds left in multipass-arena, giving
//   O(n) tree depth. Here we find the +/* nearest the midpoint and split there,
//   which recurses into balanced halves: O(log n) depth.
//   If the entire chain has no +/*, left-fold is preserved (all - or all /).
//
//   Fork-join: rhs is std::async'd when the token range exceeds kForkThreshold
//   AND the fork-depth budget > 0. lhs runs in the calling thread; both halves
//   are complete before the parent emits its node. Thread count: ≤ 2^forkDepth.
//
// Thread safety:
//   tokens_, candsByDepth_, parenMatch_ are written only in buildCandidates()
//   (single-threaded) and are read-only during all parseRange() calls.
//   nodes_ is pre-sized to tokens_.size() before any thread starts.
//   emit() atomically claims a slot via fetch_add; children are always emitted
//   before the parent (fut.get() precedes the parent's emit), so tree links
//   are valid when the evaluator walks them.
#include "parser/arena_ast.hpp"
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mp {
namespace {

// ---- grammar helpers (mirror multipass_arena.cpp) --------------------------
int binPrec(TokenType t) {
    switch (t) {
        case TokenType::Plus:
        case TokenType::Minus:  return 1;
        case TokenType::Star:
        case TokenType::Slash:  return 2;
        case TokenType::Caret:  return 4;
        default:                return -1;
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

struct ByPos {
    bool operator()(const Candidate& c, std::size_t v) const { return c.pos < v; }
};

// ---- configurable parallel evaluator ---------------------------------------
class MultipassParallel final : public IEvaluator {
    using CIt = std::vector<Candidate>::const_iterator;

    // fork when token range exceeds this (avoids spawning for tiny sub-trees)
    static constexpr std::size_t kForkThreshold = 128;

    const int   maxForkDepth_;
    const char* name_;

    // read-only after buildCandidates()
    std::vector<Token>                  tokens_;
    std::vector<std::vector<Candidate>> candsByDepth_;
    std::vector<std::size_t>            parenMatch_;

    // pre-sized before parse; atomic index enables lock-free multi-thread emit
    std::vector<ArenaAst::Node> nodes_;
    std::atomic<int>            nextNode_{0};
    const double*               vars_ = nullptr;

    // ---- node allocation ---------------------------------------------------
    int emit(ArenaAst::Node nd) {
        const int idx = nextNode_.fetch_add(1, std::memory_order_relaxed);
        nodes_[static_cast<std::size_t>(idx)] = nd;
        return idx;
    }

    // ---- O(n) setup pass ---------------------------------------------------
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
                    ++depth; expectOperand = true; break;
                case TokenType::RParen:
                    if (!stack.empty()) {
                        const std::size_t open = stack.back(); stack.pop_back();
                        parenMatch_[open] = i; parenMatch_[i] = open;
                    }
                    --depth; expectOperand = false; break;
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
                    expectOperand = true; break;
                }
                default: break;
            }
        }
    }

    std::pair<CIt, CIt> candRange(std::size_t lo, std::size_t hi, int depth) const {
        static const std::vector<Candidate> empty;
        const auto& v = (depth >= 0 && depth < static_cast<int>(candsByDepth_.size()))
                        ? candsByDepth_[static_cast<std::size_t>(depth)] : empty;
        auto b = std::lower_bound(v.begin(), v.end(), lo, ByPos{});
        auto e = std::lower_bound(b,         v.end(), hi, ByPos{});
        return {b, e};
    }

    // ---- split-point selection ---------------------------------------------
    // Returns the min-prec candidate closest to mid whose operator is associative
    // (+/*). Falls back to rightmost min-prec if no associative candidate exists.
    CIt findSplit(CIt beg, CIt end, std::size_t lo, std::size_t hi) const {
        if (beg == end) return end;

        // Determine minimum precedence.
        int minPrec = std::numeric_limits<int>::max();
        for (auto it = beg; it != end; ++it)
            if (!it->unary || it->pos == lo)
                minPrec = std::min(minPrec, it->prec);
        if (minPrec == std::numeric_limits<int>::max()) return end;

        const std::size_t mid = (lo + hi) / 2;

        // First pass: associative (+/*) nearest midpoint.
        CIt   best     = end;
        std::size_t bestDist = std::numeric_limits<std::size_t>::max();
        for (auto it = beg; it != end; ++it) {
            if (it->unary) continue;                         // skip unary
            if (it->prec != minPrec || it->rightAssoc) continue;
            const auto tt = tokens_[it->pos].type;
            if (tt != TokenType::Plus && tt != TokenType::Star) continue;
            const std::size_t d = (it->pos >= mid) ? it->pos - mid : mid - it->pos;
            if (d < bestDist) { bestDist = d; best = it; }
        }
        if (best != end) return best;

        // Fallback: rightmost min-prec (preserves left-assoc for -, /, handles ^).
        for (auto it = end; it != beg; ) {
            --it;
            if (it->unary && it->pos != lo) continue;
            if (best == end || it->prec < best->prec) {
                best = it;
                if (best->prec == 1 && !best->rightAssoc) break;
            } else if (it->prec == best->prec && best->rightAssoc) {
                best = it;
            }
        }
        return best;
    }

    // Find the +/* in [cbeg,cend) nearest the token midpoint (hi+lo)/2.
    // Returns cend if none (chain is all -//).
    CIt findChainBisect(CIt cbeg, CIt cend, std::size_t lo, std::size_t hi) const {
        const std::size_t mid = (lo + hi) / 2;
        CIt   best     = cend;
        std::size_t bestDist = std::numeric_limits<std::size_t>::max();
        for (auto it = cbeg; it != cend; ++it) {
            if (it->unary) continue;
            const auto tt = tokens_[it->pos].type;
            if (tt != TokenType::Plus && tt != TokenType::Star) continue;
            const std::size_t d = (it->pos >= mid) ? it->pos - mid : mid - it->pos;
            if (d < bestDist) { bestDist = d; best = it; }
        }
        return best;
    }

    // ---- recursive parser --------------------------------------------------
    int parseRange(std::size_t lo, std::size_t hi, int depth,
                   CIt cbeg, CIt cend, int forkDepth) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        // ---- flat-chain detection ------------------------------------------
        if (cbeg != cend) {
            const int  chainPrec = cbeg->prec;
            const bool chainRa   = cbeg->rightAssoc;
            bool flat = true;
            for (auto it = cbeg; it != cend; ++it)
                if (it->unary || it->prec != chainPrec) { flat = false; break; }

            if (flat) {
                if (!chainRa) {
                    // Bisect at nearest +/* when available.
                    const CIt splitAt = findChainBisect(cbeg, cend, lo, hi);
                    if (splitAt != cend) {
                        // Two independent sub-chains; fork rhs if budget allows.
                        if (forkDepth > 0 && hi - lo > kForkThreshold) {
                            const int fd = forkDepth - 1;
                            auto fut = std::async(std::launch::async,
                                [this, splitAt, hi, depth, cend, fd] {
                                    return parseRange(splitAt->pos + 1, hi, depth,
                                                      splitAt + 1, cend, fd);
                                });
                            int lhs = parseRange(lo, splitAt->pos, depth,
                                                 cbeg, splitAt, fd);
                            int rhs = fut.get();
                            return emit({binaryKind(tokens_[splitAt->pos].type),
                                         lhs, rhs, 0, 0.0});
                        }
                        int lhs = parseRange(lo, splitAt->pos, depth, cbeg, splitAt, 0);
                        int rhs = parseRange(splitAt->pos + 1, hi, depth,
                                             splitAt + 1, cend, 0);
                        return emit({binaryKind(tokens_[splitAt->pos].type),
                                     lhs, rhs, 0, 0.0});
                    }
                    // All -, /: left-fold (can't re-associate safely).
                    int acc = parseRange(lo, cbeg->pos, depth, cend, cend, 0);
                    for (auto it = cbeg; it != cend; ++it) {
                        const std::size_t nlo = it->pos + 1;
                        const std::size_t nhi = (it + 1 != cend) ? (it+1)->pos : hi;
                        int rhs = parseRange(nlo, nhi, depth, cend, cend, 0);
                        acc = emit({binaryKind(tokens_[it->pos].type), acc, rhs, 0, 0.0});
                    }
                    return acc;
                } else {
                    // Right fold (only ^ reaches here): unchanged.
                    std::vector<int> parts;
                    parts.reserve(static_cast<std::size_t>(cend - cbeg) + 1);
                    std::size_t prevLo = lo;
                    for (auto it = cbeg; it != cend; ++it) {
                        parts.push_back(parseRange(prevLo, it->pos, depth, cend, cend, 0));
                        prevLo = it->pos + 1;
                    }
                    parts.push_back(parseRange(prevLo, hi, depth, cend, cend, 0));
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

        // ---- general split -------------------------------------------------
        const CIt splitIt = findSplit(cbeg, cend, lo, hi);
        if (splitIt != cend) {
            if (splitIt->unary) {
                int operand = parseRange(splitIt->pos + 1, hi, depth,
                                         splitIt + 1, cend, forkDepth);
                const ArenaAst::K k = (tokens_[splitIt->pos].type == TokenType::Minus)
                                      ? ArenaAst::K::Neg : ArenaAst::K::Pos;
                return emit({k, operand, -1, 0, 0.0});
            }
            if (forkDepth > 0 && hi - lo > kForkThreshold) {
                const int fd = forkDepth - 1;
                auto fut = std::async(std::launch::async,
                    [this, splitIt, hi, depth, cend, fd] {
                        return parseRange(splitIt->pos + 1, hi, depth,
                                          splitIt + 1, cend, fd);
                    });
                int lhs = parseRange(lo, splitIt->pos, depth, cbeg, splitIt, fd);
                int rhs = fut.get();
                return emit({binaryKind(tokens_[splitIt->pos].type), lhs, rhs, 0, 0.0});
            }
            int lhs = parseRange(lo,               splitIt->pos, depth, cbeg,        splitIt, forkDepth);
            int rhs = parseRange(splitIt->pos + 1, hi,           depth, splitIt + 1, cend,    forkDepth);
            return emit({binaryKind(tokens_[splitIt->pos].type), lhs, rhs, 0, 0.0});
        }

        // ---- paren group or leaf -------------------------------------------
        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi - 1) {
            auto [b2, e2] = candRange(lo + 1, hi - 1, depth + 1);
            return parseRange(lo + 1, hi - 1, depth + 1, b2, e2, forkDepth);
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

    // ---- tree evaluation (avoids moving nodes_ out; preserves capacity) ----
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
        return 0.0;
    }

public:
    explicit MultipassParallel(int maxForkDepth, const char* name)
        : maxForkDepth_(maxForkDepth), name_(name) {}

    const char* name() const override { return name_; }

    double eval(std::string_view src, const double* vars = nullptr) override {
        tokens_ = tokenize(src);
        vars_ = vars;
        nextNode_.store(0, std::memory_order_relaxed);
        // resize() reuses existing capacity after the first call — no realloc.
        nodes_.resize(tokens_.size());
        buildCandidates();
        const std::size_t n = tokens_.size() - 1;
        auto [cbeg, cend] = candRange(0, n, 0);
        const int root = parseRange(0, n, 0, cbeg, cend, maxForkDepth_);
        return evalNode(root);
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_multipass_par1() {
    return std::make_unique<MultipassParallel>(0, "multipass-par1");
}
std::unique_ptr<IEvaluator> make_multipass_par2() {
    return std::make_unique<MultipassParallel>(1, "multipass-par2");
}
std::unique_ptr<IEvaluator> make_multipass_par4() {
    return std::make_unique<MultipassParallel>(2, "multipass-par4");
}
std::unique_ptr<IEvaluator> make_multipass_par8() {
    return std::make_unique<MultipassParallel>(3, "multipass-par8");
}

}  // namespace mp
