// multipass_lean.cpp — divide-and-conquer direct-eval (no AST).
//
// The arena-AST variants build nodes_ then walk it in a second O(n) pass.
// direct-mp collapses both passes: the recursion returns a double directly,
// so there is no intermediate tree and no evalNode overhead.
//
// Improvements in this version:
//
//   #7  Bounded scans + precedence buckets (same as other multipass variants):
//       cap Theta(n^2) worst cases at O(n log n).
//
//   #8  Index passing: the split already passes (d, clo, chi) slices to sub-
//       ranges, avoiding a binary search on every call (only paren strips need
//       one, via parenRange which uses the precomputed pcStart_/pcEnd_ arrays).
//
//   #9  AVX2 SIMD flat check and split (same design as multipass-arena):
//       a per-depth int8_t prec array lets a single 32-byte load answer both
//       questions branchlessly. Runtime-dispatched; set MP_LEAN_NO_SIMD=1 to
//       force the scalar path.

#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"
#include "parser/token.hpp"

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

// ─────────────────────────────────────────────── shared helpers ────────────

static int binPrec(TokenType t) {
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

[[gnu::always_inline]] static double applyBinary(TokenType t, double l, double r) {
    switch (t) {
        case TokenType::Plus:  return l + r;
        case TokenType::Minus: return l - r;
        case TokenType::Star:  return l * r;
        case TokenType::Slash: return l / r;
        case TokenType::Caret: return std::pow(l, r);
        default: throw std::runtime_error("bad binary op");
    }
}
[[gnu::always_inline]] static double applyUnary(TokenType t, double v) {
    return (t == TokenType::Minus) ? -v : v;
}

// Compact candidate — 8 bytes vs 16 in the arena variant.
struct Cand {
    uint32_t pos;
    int8_t   prec;
    bool     rightAssoc;
    bool     unary;
    uint8_t  _pad{};
};

// Sorted token positions of one depth's candidates, split by precedence class
// — the O(log n) fallback once a linear scan exceeds its budget.
struct Buckets {
    std::vector<uint32_t> p1, p2, caret, un;
};

constexpr int kScanBudget = 16;
constexpr uint32_t kNoPos = UINT32_MAX;

static uint32_t rightmostIn(const std::vector<uint32_t>& v,
                             uint32_t lo, uint32_t hi) {
    auto it = std::lower_bound(v.begin(), v.end(), hi);
    if (it == v.begin()) return kNoPos;
    --it;
    return (*it >= lo) ? *it : kNoPos;
}

static bool anyIn(const std::vector<uint32_t>& v, uint32_t lo, uint32_t hi) {
    auto it = std::lower_bound(v.begin(), v.end(), lo);
    return it != v.end() && *it < hi;
}

// ── AVX2 SIMD window (runtime-dispatched) ──────────────────────────────────
#if defined(__x86_64__)
#define MP_LEAN_SIMD 1
#include <cstdlib>
#include <immintrin.h>

const bool kHasSIMD = __builtin_cpu_supports("avx2") &&
                      std::getenv("MP_LEAN_NO_SIMD") == nullptr;

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

// ───────────────────────────────────────────── shared base ─────────────────

class LeanBase {
protected:
    std::vector<Token>               tokens_;
    std::vector<std::vector<Cand>>   cands_;
    std::vector<Buckets>             bucks_;
#if MP_LEAN_SIMD
    std::vector<std::vector<int8_t>> prec_;   // mirrors cands_: prec byte per candidate
#endif
    std::vector<uint32_t>            parenMatch_;
    std::vector<uint32_t>            pcStart_;
    std::vector<uint32_t>            pcEnd_;
    const double*                    vars_ = nullptr;

    void buildAll_full() {
        const auto n = (uint32_t)tokens_.size();
        parenMatch_.assign(n, 0);
        pcStart_.assign(n, 0);
        pcEnd_.assign(n, 0);
        for (auto& v : cands_) v.clear();
        for (auto& b : bucks_) {
            b.p1.clear(); b.p2.clear(); b.caret.clear(); b.un.clear();
        }
#if MP_LEAN_SIMD
        for (auto& p : prec_) p.clear();
#endif
        int depth = 0;
        bool expectOperand = true;
        std::vector<uint32_t> stk;
        for (uint32_t i = 0; i < n - 1; ++i) {
            switch (tokens_[i].type) {
                case TokenType::LParen:
                    stk.push_back(i); ++depth;
                    if (depth >= (int)cands_.size()) {
                        cands_.resize(depth+1);
                        bucks_.resize(depth+1);
#if MP_LEAN_SIMD
                        prec_.resize(depth+1);
#endif
                    }
                    pcStart_[i] = (uint32_t)cands_[depth].size();
                    expectOperand = true;
                    break;
                case TokenType::RParen: {
                    if (!stk.empty()) {
                        uint32_t open = stk.back(); stk.pop_back();
                        parenMatch_[open] = i; parenMatch_[i] = open;
                        if (depth < (int)cands_.size())
                            pcEnd_[open] = pcEnd_[i] = (uint32_t)cands_[depth].size();
                    }
                    --depth; expectOperand = false;
                    break;
                }
                case TokenType::Number:
                case TokenType::Ident: expectOperand = false; break;
                default: {
                    TokenType ty = tokens_[i].type;
                    Cand c; c.pos = i;
                    if (expectOperand) {
                        if (ty != TokenType::Plus && ty != TokenType::Minus)
                            throw std::runtime_error("unexpected operator");
                        c.prec = kUnaryPrec; c.rightAssoc = false; c.unary = true;
                    } else {
                        c.prec = (int8_t)binPrec(ty);
                        c.rightAssoc = (ty == TokenType::Caret);
                        c.unary = false;
                    }
                    if (depth >= (int)cands_.size()) {
                        cands_.resize(depth+1);
                        bucks_.resize(depth+1);
#if MP_LEAN_SIMD
                        prec_.resize(depth+1);
#endif
                    }
                    cands_[depth].push_back(c);
#if MP_LEAN_SIMD
                    prec_[depth].push_back(c.prec);
#endif
                    auto& bk = bucks_[depth];
                    if (c.unary)          bk.un.push_back(i);
                    else if (c.prec == 1) bk.p1.push_back(i);
                    else if (c.prec == 2) bk.p2.push_back(i);
                    else                  bk.caret.push_back(i);
                    expectOperand = true;
                    break;
                }
            }
        }
#if MP_LEAN_SIMD
        for (auto& p : prec_) p.insert(p.end(), 32, 0);
#endif
    }

    [[gnu::always_inline]] double evalLeaf(uint32_t lo, uint32_t hi) const {
        if (hi - lo != 1)
            throw std::runtime_error("syntax error at " + std::to_string(tokens_[lo].pos));
        const Token& t = tokens_[lo];
        if (t.type == TokenType::Number) return t.value;
        if (t.type == TokenType::Ident)  return vars_ ? vars_[static_cast<int>(t.value)] : 0.0;
        throw std::runtime_error("syntax error at " + std::to_string(t.pos));
    }

    std::pair<int,int> parenRange(uint32_t lo, int nd) const {
        if (nd < (int)cands_.size())
            return {(int)pcStart_[lo], (int)pcEnd_[lo]};
        return {0, 0};
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Variant 1 — direct-mp  (recursive, no AST)
// ═══════════════════════════════════════════════════════════════════════════
class DirectMp final : public LeanBase, public IEvaluator {
public:
    const char* name() const override { return "direct-mp"; }
    double eval(std::string_view src, const double* vars = nullptr) override {
        tokens_ = tokenize(src);
        vars_ = vars;
        buildAll_full();
        const uint32_t n = (uint32_t)(tokens_.size() - 1);
        int clo = 0, chi = cands_.empty() ? 0 : (int)cands_[0].size();
        return er(0, n, 0, clo, chi);
    }
private:
    int findSplitBuckets(int d, int clo, int chi) const {
        auto& v = cands_[d];
        const auto& bk = bucks_[d];
        const uint32_t blo = v[clo].pos, bhi = v[chi-1].pos + 1;
        uint32_t p = rightmostIn(bk.p1, blo, bhi);
        if (p == kNoPos) p = rightmostIn(bk.p2, blo, bhi);
        if (p == kNoPos) return clo;
        return (int)(std::lower_bound(v.begin()+clo, v.begin()+chi, p,
                         [](const Cand& c, uint32_t x){ return c.pos < x; })
                     - v.begin());
    }

    bool flatByBuckets(int d, int clo, int chi) const {
        auto& v = cands_[d];
        const auto& bk = bucks_[d];
        const uint32_t blo = v[clo].pos, bhi = v[chi-1].pos + 1;
        if (anyIn(bk.un, blo, bhi)) return false;
        const int classes = (anyIn(bk.p1, blo, bhi) ? 1 : 0) +
                            (anyIn(bk.p2, blo, bhi) ? 1 : 0) +
                            (anyIn(bk.caret, blo, bhi) ? 1 : 0);
        return classes == 1;
    }

    int findSplit(int d, int clo, int chi, uint32_t lo) const {
        if (clo >= chi) return -1;
        auto& v = cands_[d];
        int best = -1;
        const int stop = (chi - clo > kScanBudget) ? chi - kScanBudget : clo;
        for (int i = chi-1; i >= stop; --i) {
            if (v[i].unary && v[i].pos != lo) continue;
            if (best < 0 || v[i].prec < v[best].prec) {
                best = i;
                if (v[best].prec == 1 && !v[best].rightAssoc) return best;
            } else if (v[i].prec == v[best].prec && v[best].rightAssoc) {
                best = i;
            }
        }
        if (stop == clo) return best;
        return findSplitBuckets(d, clo, chi);
    }

    double er(uint32_t lo, uint32_t hi, int d, int clo, int chi) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");
        if (clo < chi) {
            auto& v = cands_[d];
            int cp = v[clo].prec; bool cr = v[clo].rightAssoc;

            bool flat;
#if MP_LEAN_SIMD
            if (kHasSIMD) {
                const int f = simdWindowFlat(prec_[d].data(), clo, chi);
                flat = (f < 0) ? flatByBuckets(d, clo, chi) : (f != 0);
            } else
#endif
            {
                flat = true;
                const int scanEnd = (chi - clo > kScanBudget) ? clo + kScanBudget : chi;
                for (int i = clo; i < scanEnd; ++i)
                    if (v[i].unary || v[i].prec != cp) { flat = false; break; }
                if (flat && scanEnd != chi)
                    flat = flatByBuckets(d, clo, chi);
            }

            if (flat) {
                if (!cr) {
                    double acc = er(lo, v[clo].pos, d, chi, chi);
                    for (int i = clo; i < chi; ++i) {
                        uint32_t nhi = (i+1<chi) ? v[i+1].pos : hi;
                        acc = applyBinary(tokens_[v[i].pos].type, acc,
                                          er(v[i].pos+1, nhi, d, chi, chi));
                    }
                    return acc;
                } else {
                    double acc = er(v[chi-1].pos+1, hi, d, chi, chi);
                    for (int i = chi-1; i >= clo; --i) {
                        uint32_t slo = (i == clo) ? lo : v[i-1].pos+1;
                        acc = applyBinary(tokens_[v[i].pos].type,
                                          er(slo, v[i].pos, d, chi, chi), acc);
                    }
                    return acc;
                }
            }

            int si;
#if MP_LEAN_SIMD
            if (kHasSIMD) {
                const long r = simdWindowSplit(prec_[d].data(), clo, chi);
                si = (r == kWinBuckets) ? findSplitBuckets(d, clo, chi)
                   : (r == kWinFirst)   ? clo
                                        : (int)r;
            } else
#endif
            si = findSplit(d, clo, chi, lo);

            if (si >= 0) {
                auto& c = cands_[d][si];
                if (c.unary)
                    return applyUnary(tokens_[c.pos].type, er(c.pos+1, hi, d, si+1, chi));
                return applyBinary(tokens_[c.pos].type,
                                   er(lo, c.pos, d, clo, si),
                                   er(c.pos+1, hi, d, si+1, chi));
            }
        }
        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi-1) {
            int nd = d+1; auto [nb, ne] = parenRange(lo, nd);
            return er(lo+1, hi-1, nd, nb, ne);
        }
        return evalLeaf(lo, hi);
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_direct_mp() { return std::make_unique<DirectMp>(); }

}  // namespace mp
