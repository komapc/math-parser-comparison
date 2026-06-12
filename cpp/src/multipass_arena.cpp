#include "parser/arena_ast.hpp"
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
// The two linear scans (findSplit and the flat-chain check) are BOUNDED: past
// kScanBudget candidates without an answer, the rest comes from per-precedence
// sorted position arrays ("buckets") via binary search. Common ranges never
// notice (they resolve within the budget, same code path as before); the
// adversarial chains that used to be Theta(n^2) — mixed-precedence powchain
// (no prec-1 early exit) and towerchain (long same-precedence prefix rescanned
// by the flat check before every split) — drop to O(n log n). See
// bench/adversarial_bench.cpp.

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

// Sorted token positions of one depth's candidates, split by precedence class
// — the O(log n) fallback once a linear scan exceeds its budget.
struct Buckets {
    std::vector<uint32_t> p1, p2, caret, un;
};

// Linear scans give up after this many candidates and consult the buckets.
constexpr std::ptrdiff_t kScanBudget = 16;

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// Rightmost position in sorted `v` within [lo, hi), or kNone.
std::size_t rightmostIn(const std::vector<uint32_t>& v,
                        std::size_t lo, std::size_t hi) {
    auto it = std::lower_bound(v.begin(), v.end(), static_cast<uint32_t>(hi));
    if (it == v.begin()) return kNone;
    --it;
    return (*it >= lo) ? static_cast<std::size_t>(*it) : kNone;
}

bool anyIn(const std::vector<uint32_t>& v, std::size_t lo, std::size_t hi) {
    auto it = std::lower_bound(v.begin(), v.end(), static_cast<uint32_t>(lo));
    return it != v.end() && *it < hi;
}

// ── SIMD WINDOW SCAN (AVX2, runtime-dispatched) ─────────────────────────────
// A per-depth byte array mirrors the candidate list: 1 / 2 for binary + - and
// * /, 3 for unary, 4 for ^ (padded with 32 zeros so unaligned loads stay in
// bounds). One 32-byte load over the RIGHTMOST candidates of a range answers
// the same questions as the scalar loops, branchlessly:
//   prec==1 mask, highest set bit  → rightmost + - (the usual split);
//   range ≤ 32: prec==2 mask       → rightmost * /; both empty → the range's
//               first candidate (leading unary or leftmost right-assoc ^);
//   range > 32 with no + - in the window → bucket fallback (O(log n)).
// Flatness for ranges ≤ 32 is one compare against the first prec broadcast.
#if defined(__x86_64__)
#define MP_ARENA_SIMD 1
#include <immintrin.h>
#include <cstdlib>

// MP_ARENA_NO_SIMD=1 forces the scalar path — lets an A/B benchmark compare
// both paths inside one binary (no code-layout luck between two builds).
const bool kHasAvx2 = __builtin_cpu_supports("avx2") &&
                      std::getenv("MP_ARENA_NO_SIMD") == nullptr;

// Split decision for prec bytes [ib, ie): index of the split candidate,
// kWinFirst (use the range's first candidate), or kWinBuckets (fall back).
constexpr long kWinFirst   = -1;
constexpr long kWinBuckets = -2;

__attribute__((target("avx2")))
long simdWindowSplit(const int8_t* prec, long ib, long ie) {
    const long k    = ie - ib;
    const long base = (k >= 32) ? ie - 32 : ib;
    const uint32_t valid = (k >= 32) ? 0xFFFFFFFFu : ((1u << k) - 1);
    const __m256i v = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(prec + base));
    const auto m1 = static_cast<uint32_t>(_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(v, _mm256_set1_epi8(1)))) & valid;
    if (m1) return base + 31 - std::countl_zero(m1);
    if (k > 32) return kWinBuckets;
    const auto m2 = static_cast<uint32_t>(_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(v, _mm256_set1_epi8(2)))) & valid;
    if (m2) return base + 31 - std::countl_zero(m2);
    return kWinFirst;
}

// 1 = flat, 0 = not flat, -1 = the leading 32 are uniform but the range is
// longer (ask the buckets to vouch for the rest). Checking the window first
// keeps the common "disproved immediately" case off the bucket path.
__attribute__((target("avx2")))
int simdWindowFlat(const int8_t* prec, long ib, long ie) {
    const long k = ie - ib;
    const int8_t p0 = prec[ib];
    if (p0 == 3) return 0;                       // leading unary: not flat
    const long w = (k < 32) ? k : 32;
    const uint32_t valid = (w == 32) ? 0xFFFFFFFFu : ((1u << w) - 1);
    const __m256i v = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(prec + ib));
    const auto eq = static_cast<uint32_t>(_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(v, _mm256_set1_epi8(p0))));
    if ((eq & valid) != valid) return 0;
    return (k <= 32) ? 1 : -1;
}
#endif  // __x86_64__

class MultiPassArena final : public IEvaluator {
    using CIt = std::vector<Candidate>::const_iterator;

public:
    const char* name() const override { return "multipass-arena"; }

    // Build and return an ArenaAst (moves nodes_ out; used by reeval).
    ArenaAst buildArena(std::string_view src) {
        build(src);
        const std::size_t n = tokens_.size() - 1;
        auto [cbeg, cend] = candRange(0, n, 0);
        const int root = parseRange(0, n, 0, cbeg, cend);
        return ArenaAst::adopt(std::move(nodes_), root);
    }

    double eval(std::string_view src, const double* vars = nullptr) override {
        vars_ = vars;
        build(src);
        const std::size_t n = tokens_.size() - 1;
        auto [cbeg, cend] = candRange(0, n, 0);
        return evalNode(parseRange(0, n, 0, cbeg, cend));
    }

private:
    std::vector<Token>                  tokens_;
    std::vector<ArenaAst::Node>         nodes_;
    std::vector<std::vector<Candidate>> candsByDepth_;
    std::vector<Buckets>                bucketsByDepth_;
#if MP_ARENA_SIMD
    std::vector<std::vector<int8_t>>    precByDepth_;  // mirrors candsByDepth_
#endif
    std::vector<std::size_t>            parenMatch_;
    const double*                       vars_ = nullptr;

    void build(std::string_view src) {
        tokens_ = tokenize(src);
        nodes_.clear();
        nodes_.reserve(tokens_.size());
        buildCandidates();
    }

    // One O(n) pass: build per-depth candidate lists + precomputed paren matches.
    void buildCandidates() {
        // Clear contents but keep every inner vector's capacity — after a few
        // evals no allocation happens here at all.
        for (auto& v : candsByDepth_) v.clear();
        for (auto& b : bucketsByDepth_) {
            b.p1.clear(); b.p2.clear(); b.caret.clear(); b.un.clear();
        }
#if MP_ARENA_SIMD
        for (auto& p : precByDepth_) p.clear();
#endif
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
                    if (depth >= static_cast<int>(candsByDepth_.size())) {
                        candsByDepth_.resize(static_cast<std::size_t>(depth) + 1);
                        bucketsByDepth_.resize(static_cast<std::size_t>(depth) + 1);
#if MP_ARENA_SIMD
                        precByDepth_.resize(static_cast<std::size_t>(depth) + 1);
#endif
                    }
                    candsByDepth_[static_cast<std::size_t>(depth)].push_back(c);
#if MP_ARENA_SIMD
                    precByDepth_[static_cast<std::size_t>(depth)]
                        .push_back(static_cast<int8_t>(c.prec));
#endif
                    auto& bk = bucketsByDepth_[static_cast<std::size_t>(depth)];
                    const auto pos32 = static_cast<uint32_t>(i);
                    if (c.unary)          bk.un.push_back(pos32);
                    else if (c.prec == 1) bk.p1.push_back(pos32);
                    else if (c.prec == 2) bk.p2.push_back(pos32);
                    else                  bk.caret.push_back(pos32);
                    expectOperand = true;
                    break;
                }
                default: break;
            }
        }
#if MP_ARENA_SIMD
        // 32 zero bytes of padding so window loads never read out of bounds
        // (zeros match no precedence class).
        for (auto& p : precByDepth_) p.insert(p.end(), 32, 0);
#endif
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

    // Bucket-based equivalent of the full RTL scan, O(log n): the rightmost
    // prec-1 candidate, else the rightmost prec-2, else the range's first
    // candidate — which is then either a leading unary (lowest precedence
    // present) or the leftmost ^ (right-assoc). A unary candidate that is not
    // at the very start of the range cannot be first: the token before it is
    // a same-depth operator, which would itself be the earlier candidate.
    CIt findSplitBuckets(CIt beg, CIt end, int depth) const {
        const auto& bk = bucketsByDepth_[static_cast<std::size_t>(depth)];
        const std::size_t blo = beg->pos, bhi = end[-1].pos + 1;
        std::size_t p = rightmostIn(bk.p1, blo, bhi);
        if (p == kNone) p = rightmostIn(bk.p2, blo, bhi);
        if (p == kNone) return beg;
        return std::lower_bound(beg, end, p, ByPos{});
    }

    // True iff every candidate in [beg, end) is binary with one precedence.
    // Answered from the buckets so a long flat prefix is never rescanned.
    bool flatByBuckets(CIt beg, CIt end, int depth) const {
        const auto& bk = bucketsByDepth_[static_cast<std::size_t>(depth)];
        const std::size_t blo = beg->pos, bhi = end[-1].pos + 1;
        if (anyIn(bk.un, blo, bhi)) return false;
        const int classes = (anyIn(bk.p1, blo, bhi) ? 1 : 0) +
                            (anyIn(bk.p2, blo, bhi) ? 1 : 0) +
                            (anyIn(bk.caret, blo, bhi) ? 1 : 0);
        return classes == 1;
    }

    // Returns iterator to the split candidate, or `end` if none.
    // RTL scan: left-assoc tie-break (rightmost) = free; early exit at prec==1.
    // Bounded: past kScanBudget candidates without a prec-1 hit, the answer
    // comes from the buckets instead — O(log n), never O(k).
    CIt findSplit(CIt beg, CIt end, std::size_t lo, int depth) const {
        if (beg == end) return end;
        CIt best = end;
        const CIt stop = (end - beg > kScanBudget) ? end - kScanBudget : beg;
        for (auto it = end; it != stop; ) {
            --it;
            if (it->unary && it->pos != lo) continue;
            if (best == end || it->prec < best->prec) {
                best = it;
                if (best->prec == 1 && !best->rightAssoc) return best;
            } else if (it->prec == best->prec && best->rightAssoc) {
                best = it;  // right-assoc: prefer leftmost
            }
        }
        if (stop == beg) return best;     // scanned everything: exact answer
        return findSplitBuckets(beg, end, depth);
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
            // Hybrid flatness check: scan a bounded prefix first — a mixed
            // range is usually disproved within a few candidates (old cost).
            // Only a uniform prefix longer than the budget asks the buckets
            // to vouch for the rest (O(log n), never an O(k) rescan).
            bool flat;
#if MP_ARENA_SIMD
            if (kHasAvx2) {
                const auto& dv = candsByDepth_[static_cast<std::size_t>(depth)];
                const int f = simdWindowFlat(
                    precByDepth_[static_cast<std::size_t>(depth)].data(),
                    cbeg - dv.begin(), cend - dv.begin());
                flat = (f < 0) ? flatByBuckets(cbeg, cend, depth) : (f != 0);
            } else
#endif
            {
                flat = true;
                const CIt scanEnd =
                    (cend - cbeg > kScanBudget) ? cbeg + kScanBudget : cend;
                for (auto it = cbeg; it != scanEnd; ++it)
                    if (it->unary || it->prec != chainPrec) { flat = false; break; }
                if (flat && scanEnd != cend)
                    flat = flatByBuckets(cbeg, cend, depth);
            }

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
        CIt splitIt;
#if MP_ARENA_SIMD
        if (kHasAvx2) {
            if (cbeg == cend) {
                splitIt = cend;
            } else {
                const auto& dv = candsByDepth_[static_cast<std::size_t>(depth)];
                const long r = simdWindowSplit(
                    precByDepth_[static_cast<std::size_t>(depth)].data(),
                    cbeg - dv.begin(), cend - dv.begin());
                splitIt = (r == kWinBuckets) ? findSplitBuckets(cbeg, cend, depth)
                        : (r == kWinFirst)   ? cbeg
                                             : dv.begin() + r;
            }
        } else
#endif
        splitIt = findSplit(cbeg, cend, lo, depth);
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

std::unique_ptr<IEvaluator> make_ast_multipass_arena() {
    return std::make_unique<MultiPassArena>();
}

ArenaAst multipass_arena_parse(std::string_view src) {
    MultiPassArena p;
    return p.buildArena(src);
}

}  // namespace mp
