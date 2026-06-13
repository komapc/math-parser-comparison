#include "parser/ast.hpp"
#include "parser/lexer.hpp"
#include "parser/parser.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// Divide-and-conquer parser. Full complexity analysis in multipass_arena.cpp.
//
// This variant (pointer-AST) adds two improvements over the previous version:
//
//   #8  Iterator passing (ported from multipass-arena): when parseRange splits
//       at candidate k, the two sub-ranges' candidate spans are already known —
//       the left half gets [cbeg, splitIt) and the right gets [splitIt+1, cend).
//       Flat-chain sub-ranges between chain operators have ZERO depth-d
//       candidates and receive (cend, cend). The only call that still needs a
//       binary search is a paren strip (depth change). Before this fix every
//       recursive call did a binary search: O(log n) per call, O(n log² n)
//       total. Now it is O(n log n) like the arena variant.
//
//   #9  AVX2 SIMD window (same design as multipass-arena): a per-depth int8_t
//       prec array lets a single 32-byte load answer both the flat check and the
//       split decision branchlessly. Compile-time gated on __x86_64__; runtime-
//       dispatched so the same binary runs on non-AVX2 machines. Set
//       MP_NO_SIMD=1 to force the scalar path for A/B comparison.

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

struct Candidate {
    std::size_t pos;
    int         prec;
    bool        rightAssoc;
    bool        unary;
};

struct Buckets {
    std::vector<std::size_t> p1, p2, caret, un;
};

constexpr std::ptrdiff_t kScanBudget = 16;
constexpr std::size_t    kNone       = static_cast<std::size_t>(-1);

std::size_t rightmostIn(const std::vector<std::size_t>& v,
                        std::size_t lo, std::size_t hi) {
    auto it = std::lower_bound(v.begin(), v.end(), hi);
    if (it == v.begin()) return kNone;
    --it;
    return (*it >= lo) ? *it : kNone;
}

bool anyIn(const std::vector<std::size_t>& v, std::size_t lo, std::size_t hi) {
    auto it = std::lower_bound(v.begin(), v.end(), lo);
    return it != v.end() && *it < hi;
}

// ── AVX2 SIMD window (runtime-dispatched) ──────────────────────────────────
#if defined(__x86_64__)
#define MP_SIMD 1
#include <cstdlib>
#include <immintrin.h>

const bool kHasSIMD = __builtin_cpu_supports("avx2") &&
                      std::getenv("MP_NO_SIMD") == nullptr;

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

__attribute__((target("avx2")))
int simdWindowFlat(const int8_t* prec, long ib, long ie) {
    const long k = ie - ib;
    const int8_t p0 = prec[ib];
    if (p0 == 3) return 0;
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

class MultiPass final : public IParser {
    using CIt = std::vector<Candidate>::const_iterator;

public:
    const char* name() const override { return "multipass"; }

    ExprPtr parse(std::string_view src) override {
        tokens_ = tokenize(src);
        buildCandidates();
        const std::size_t n = tokens_.size() - 1;
        auto [cbeg, cend] = candRange(0, n, 0);
        return parseRange(0, n, 0, cbeg, cend);
    }

private:
    std::vector<Token>                  tokens_;
    std::vector<std::vector<Candidate>> candsByDepth_;
    std::vector<Buckets>                bucketsByDepth_;
#if MP_SIMD
    std::vector<std::vector<int8_t>>    precByDepth_;
#endif
    std::vector<std::size_t>            parenMatch_;

    void buildCandidates() {
        for (auto& v : candsByDepth_) v.clear();
        for (auto& b : bucketsByDepth_) {
            b.p1.clear(); b.p2.clear(); b.caret.clear(); b.un.clear();
        }
#if MP_SIMD
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
                    const std::size_t d = static_cast<std::size_t>(depth);
                    if (depth >= static_cast<int>(candsByDepth_.size())) {
                        candsByDepth_.resize(d + 1);
                        bucketsByDepth_.resize(d + 1);
#if MP_SIMD
                        precByDepth_.resize(d + 1);
#endif
                    }
                    candsByDepth_[d].push_back(c);
#if MP_SIMD
                    precByDepth_[d].push_back(static_cast<int8_t>(c.prec));
#endif
                    auto& bk = bucketsByDepth_[d];
                    if (c.unary)          bk.un.push_back(i);
                    else if (c.prec == 1) bk.p1.push_back(i);
                    else if (c.prec == 2) bk.p2.push_back(i);
                    else                  bk.caret.push_back(i);
                    expectOperand = true;
                    break;
                }
                default: break;
            }
        }
#if MP_SIMD
        for (auto& p : precByDepth_) p.insert(p.end(), 32, 0);
#endif
    }

    struct ByPos {
        bool operator()(const Candidate& c, std::size_t v) const { return c.pos < v; }
    };

    // O(log n) — only called on the initial parse and paren strips.
    std::pair<CIt, CIt> candRange(std::size_t lo, std::size_t hi, int depth) const {
        static const std::vector<Candidate> empty;
        const auto& v = (depth >= 0 && depth < static_cast<int>(candsByDepth_.size()))
                        ? candsByDepth_[static_cast<std::size_t>(depth)] : empty;
        auto b = std::lower_bound(v.begin(), v.end(), lo, ByPos{});
        auto e = std::lower_bound(b,         v.end(), hi, ByPos{});
        return {b, e};
    }

    CIt findSplitBuckets(CIt beg, CIt end, int depth) const {
        const auto& bk = bucketsByDepth_[static_cast<std::size_t>(depth)];
        const std::size_t blo = beg->pos, bhi = end[-1].pos + 1;
        std::size_t p = rightmostIn(bk.p1, blo, bhi);
        if (p == kNone) p = rightmostIn(bk.p2, blo, bhi);
        if (p == kNone) return beg;
        return std::lower_bound(beg, end, p, ByPos{});
    }

    bool flatByBuckets(CIt beg, CIt end, int depth) const {
        const auto& bk = bucketsByDepth_[static_cast<std::size_t>(depth)];
        const std::size_t blo = beg->pos, bhi = end[-1].pos + 1;
        if (anyIn(bk.un, blo, bhi)) return false;
        const int classes = (anyIn(bk.p1, blo, bhi) ? 1 : 0) +
                            (anyIn(bk.p2, blo, bhi) ? 1 : 0) +
                            (anyIn(bk.caret, blo, bhi) ? 1 : 0);
        return classes == 1;
    }

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
                best = it;
            }
        }
        if (stop == beg) return best;
        return findSplitBuckets(beg, end, depth);
    }

    // cbeg/cend: already-located candidates at `depth` in [lo, hi).
    // Splits pass sliced iterators — O(1), no binary search.
    // Flat-fold sub-ranges and paren contents get (cend, cend) or a new search.
    ExprPtr parseRange(std::size_t lo, std::size_t hi, int depth,
                       CIt cbeg, CIt cend) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        if (cbeg != cend) {
            const int  chainPrec = cbeg->prec;
            const bool chainRa   = cbeg->rightAssoc;

            bool flat;
#if MP_SIMD
            if (kHasSIMD) {
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
                    ExprPtr acc = parseRange(lo, cbeg->pos, depth, cend, cend);
                    for (auto it = cbeg; it != cend; ++it) {
                        const std::size_t nextLo = it->pos + 1;
                        const std::size_t nextHi = (it + 1 != cend) ? (it+1)->pos : hi;
                        acc = binary(tokens_[it->pos].type,
                                     std::move(acc),
                                     parseRange(nextLo, nextHi, depth, cend, cend));
                    }
                    return acc;
                } else {
                    std::vector<ExprPtr> parts;
                    parts.reserve(static_cast<std::size_t>(cend - cbeg) + 1);
                    std::size_t prevLo = lo;
                    for (auto it = cbeg; it != cend; ++it) {
                        parts.push_back(parseRange(prevLo, it->pos, depth, cend, cend));
                        prevLo = it->pos + 1;
                    }
                    parts.push_back(parseRange(prevLo, hi, depth, cend, cend));
                    ExprPtr acc = std::move(parts.back());
                    auto it = cend;
                    for (std::size_t i = parts.size() - 1; i-- > 0; ) {
                        --it;
                        acc = binary(tokens_[it->pos].type,
                                     std::move(parts[i]), std::move(acc));
                    }
                    return acc;
                }
            }
        }

        // General split — iterator passed directly, no binary search.
        CIt splitIt;
#if MP_SIMD
        if (kHasSIMD) {
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
            if (splitIt->unary)
                return unary(tokens_[splitIt->pos].type,
                             parseRange(splitIt->pos + 1, hi, depth,
                                        splitIt + 1, cend));
            return binary(tokens_[splitIt->pos].type,
                          parseRange(lo,               splitIt->pos, depth, cbeg,        splitIt),
                          parseRange(splitIt->pos + 1, hi,           depth, splitIt + 1, cend));
        }

        // No depth-d operator: paren group or leaf.
        if (tokens_[lo].type == TokenType::LParen &&
            parenMatch_[lo] == hi - 1) {
            auto [b2, e2] = candRange(lo + 1, hi - 1, depth + 1);
            return parseRange(lo + 1, hi - 1, depth + 1, b2, e2);
        }
        if (hi - lo == 1) {
            if (tokens_[lo].type == TokenType::Number) return number(tokens_[lo].value);
            if (tokens_[lo].type == TokenType::Ident)
                return variable(static_cast<int>(tokens_[lo].value));
        }
        throw std::runtime_error("syntax error at position " +
                                 std::to_string(tokens_[lo].pos));
    }
};

}  // namespace

std::unique_ptr<IParser> make_multipass() {
    return std::make_unique<MultiPass>();
}

}  // namespace mp
