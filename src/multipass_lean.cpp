// multipass_lean.cpp — divide-and-conquer variants that evaluate inline.
//
// The arena-AST variants build nodes_ then walk it in a second O(n) pass.
// These variants collapse both passes: the recursion returns a double
// directly, so there is no intermediate tree and no evalNode overhead.
//
// Three variants stacked incrementally:
//
//   direct-mp          linear RTL findSplit, no sparse table, full token scan
//   direct-mp-simd     AVX2 vectorised findSplit (replaces sparse table)
//   direct-mp-full     SIMD + operator-only token scan in buildAll

#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"
#include "parser/token.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>
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

// ───────────────────────────────────────────── shared base ─────────────────

class LeanBase {
protected:
    std::vector<Token>               tokens_;
    std::vector<std::vector<Cand>>   cands_;
    std::vector<std::vector<int8_t>> precs_;    // prec-only, SIMD-friendly
    std::vector<uint32_t>            parenMatch_;
    std::vector<uint32_t>            pcStart_;  // parenCandStart
    std::vector<uint32_t>            pcEnd_;    // parenCandEnd

    // ── buildAll with full token scan (same logic as multipass_opt, no sparse table) ──
    void buildAll_full() {
        const auto n = (uint32_t)tokens_.size();
        parenMatch_.assign(n, 0);
        pcStart_.assign(n, 0);
        pcEnd_.assign(n, 0);
        cands_.clear(); precs_.clear();
        int depth = 0;
        bool expectOperand = true;
        std::vector<uint32_t> stk;
        for (uint32_t i = 0; i < n - 1; ++i) {
            switch (tokens_[i].type) {
                case TokenType::LParen:
                    stk.push_back(i); ++depth;
                    if (depth >= (int)cands_.size()) { cands_.resize(depth+1); precs_.resize(depth+1); }
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
                default: {                              // Plus Minus Star Slash Caret
                    TokenType ty = tokens_[i].type;
                    Cand c; c.pos = i;
                    if (expectOperand) {
                        if (ty != TokenType::Plus && ty != TokenType::Minus)
                            throw std::runtime_error("unexpected operator");
                        c.prec = kUnaryPrec; c.rightAssoc = false; c.unary = true;
                    } else {
                        c.prec = (int8_t)binPrec(ty); c.rightAssoc = (ty == TokenType::Caret); c.unary = false;
                    }
                    if (depth >= (int)cands_.size()) { cands_.resize(depth+1); precs_.resize(depth+1); }
                    cands_[depth].push_back(c);
                    precs_[depth].push_back(c.prec);
                    expectOperand = true;
                    break;
                }
            }
        }
    }

    // ── buildAll with operator-only scan ────────────────────────────────────
    // Skips Number/Ident tokens entirely; determines unary vs binary
    // from the preceding token type.  ~1.5× faster scan for typical
    // expressions where ~50% of tokens are leaves.
    void buildAll_oponly() {
        const auto n = (uint32_t)tokens_.size();
        parenMatch_.assign(n, 0);
        pcStart_.assign(n, 0);
        pcEnd_.assign(n, 0);
        cands_.clear(); precs_.clear();
        int depth = 0;
        std::vector<uint32_t> stk;
        for (uint32_t i = 0; i < n - 1; ++i) {
            switch (tokens_[i].type) {
                case TokenType::Number:
                case TokenType::Ident:  continue;      // leaf — skip entirely
                case TokenType::LParen:
                    stk.push_back(i); ++depth;
                    if (depth >= (int)cands_.size()) { cands_.resize(depth+1); precs_.resize(depth+1); }
                    pcStart_[i] = (uint32_t)cands_[depth].size();
                    break;
                case TokenType::RParen: {
                    if (!stk.empty()) {
                        uint32_t open = stk.back(); stk.pop_back();
                        parenMatch_[open] = i; parenMatch_[i] = open;
                        if (depth < (int)cands_.size())
                            pcEnd_[open] = pcEnd_[i] = (uint32_t)cands_[depth].size();
                    }
                    --depth;
                    break;
                }
                default: {
                    // Determine unary vs binary from the preceding token.
                    // A token that is NOT (Number | Ident | RParen) means we're in
                    // an "expect operand" context → this +/- is unary.
                    TokenType ty = tokens_[i].type;
                    bool unary;
                    if (i == 0) {
                        unary = true;
                    } else {
                        TokenType prev = tokens_[i-1].type;
                        unary = (prev != TokenType::Number &&
                                 prev != TokenType::Ident  &&
                                 prev != TokenType::RParen);
                    }
                    if (unary && ty != TokenType::Plus && ty != TokenType::Minus)
                        throw std::runtime_error("unexpected operator");
                    Cand c; c.pos = i;
                    if (unary) { c.prec = kUnaryPrec; c.rightAssoc = false; c.unary = true; }
                    else       { c.prec = (int8_t)binPrec(ty); c.rightAssoc = (ty == TokenType::Caret); c.unary = false; }
                    if (depth >= (int)cands_.size()) { cands_.resize(depth+1); precs_.resize(depth+1); }
                    cands_[depth].push_back(c);
                    precs_[depth].push_back(c.prec);
                    break;
                }
            }
        }
    }

    // ── leaf evaluation ──────────────────────────────────────────────────────
    [[gnu::always_inline]] double evalLeaf(uint32_t lo, uint32_t hi) const {
        if (hi - lo != 1) throw std::runtime_error("syntax error at " + std::to_string(tokens_[lo].pos));
        const Token& t = tokens_[lo];
        if (t.type == TokenType::Number) return t.value;
        if (t.type == TokenType::Ident)  return 0.0;
        throw std::runtime_error("syntax error at " + std::to_string(t.pos));
    }

    // ── paren-strip helper ───────────────────────────────────────────────────
    // Returns (nb, ne) — the candidate index range for depth nd inside paren at lo.
    std::pair<int,int> parenRange(uint32_t lo, int nd) const {
        if (nd < (int)cands_.size())
            return {(int)pcStart_[lo], (int)pcEnd_[lo]};
        return {0, 0};
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Variant 1 — direct-mp
// Direct evaluation with linear RTL findSplit (same logic as multipass-arena
// but no AST emitted — returns double from the recursion instead).
// ═══════════════════════════════════════════════════════════════════════════
class DirectMp final : public LeanBase, public IEvaluator {
public:
    const char* name() const override { return "direct-mp"; }
    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        buildAll_full();
        const uint32_t n = (uint32_t)(tokens_.size() - 1);
        int clo = 0, chi = cands_.empty() ? 0 : (int)cands_[0].size();
        return er(0, n, 0, clo, chi);
    }
private:
    int findSplit(int d, int clo, int chi, uint32_t lo) const {
        if (clo >= chi) return -1;
        auto& v = cands_[d];
        int best = -1;
        for (int i = chi-1; i >= clo; --i) {
            if (v[i].unary && v[i].pos != lo) continue;
            if (best < 0 || v[i].prec < v[best].prec) {
                best = i;
                if (v[best].prec == 1 && !v[best].rightAssoc) break;
            } else if (v[i].prec == v[best].prec && v[best].rightAssoc) {
                best = i;
            }
        }
        return best;
    }

    double er(uint32_t lo, uint32_t hi, int d, int clo, int chi) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");
        if (clo < chi) {
            auto& v = cands_[d];
            int cp = v[clo].prec; bool cr = v[clo].rightAssoc;
            bool flat = true;
            for (int i = clo; i < chi; ++i)
                if (v[i].unary || v[i].prec != cp) { flat = false; break; }
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
        }
        int si = findSplit(d, clo, chi, lo);
        if (si >= 0) {
            auto& c = cands_[d][si];
            if (c.unary) return applyUnary(tokens_[c.pos].type, er(c.pos+1, hi, d, si+1, chi));
            return applyBinary(tokens_[c.pos].type,
                               er(lo, c.pos, d, clo, si),
                               er(c.pos+1, hi, d, si+1, chi));
        }
        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi-1) {
            int nd = d+1; auto [nb, ne] = parenRange(lo, nd);
            return er(lo+1, hi-1, nd, nb, ne);
        }
        return evalLeaf(lo, hi);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Shared SIMD findSplit — used by variants 2 and 3.
//
// Strategy:
//   1. AVX2 horizontal min over precs_[d][clo..chi)  →  min_prec  (O(n/32))
//   2. Scalar scan to locate the correct candidate:
//        left-assoc (prec < 4): rightmost with min_prec, skip non-leading unary
//        right-assoc (prec == 4): leftmost with min_prec
//   3. If all min_prec candidates are non-leading unary, re-scan for
//      leading unary or non-unary (handles --x type expressions).
// ═══════════════════════════════════════════════════════════════════════════

[[gnu::target("avx2")]]
static int8_t simdMinPrec(const int8_t* p, int n) {
    __m256i vmin = _mm256_set1_epi8(100);
    int i = 0;
    for (; i + 32 <= n; i += 32)
        vmin = _mm256_min_epi8(vmin, _mm256_loadu_si256((const __m256i*)(p+i)));
    // horizontal reduce 256→8
    __m128i lo = _mm256_extracti128_si256(vmin, 0);
    __m128i hi = _mm256_extracti128_si256(vmin, 1);
    __m128i m  = _mm_min_epi8(lo, hi);
    m = _mm_min_epi8(m, _mm_srli_si128(m, 8));
    m = _mm_min_epi8(m, _mm_srli_si128(m, 4));
    m = _mm_min_epi8(m, _mm_srli_si128(m, 2));
    m = _mm_min_epi8(m, _mm_srli_si128(m, 1));
    int8_t best = (int8_t)_mm_extract_epi8(m, 0);
    for (; i < n; ++i) best = std::min(best, p[i]);
    return best;
}

[[gnu::target("avx2")]]
static int simdFindSplit(const std::vector<Cand>& v,
                         const std::vector<int8_t>& p,
                         int clo, int chi, uint32_t lo) {
    if (clo >= chi) return -1;
    int8_t min_prec = simdMinPrec(p.data() + clo, chi - clo);

    bool right_assoc = (min_prec == 4);  // only ^ has prec=4, always right-assoc

    if (!right_assoc) {
        // RTL: rightmost candidate with min_prec, skip non-leading unary
        for (int i = chi-1; i >= clo; --i) {
            if (p[i] != min_prec) continue;
            if (v[i].unary && v[i].pos != lo) continue;
            return i;
        }
        // All min_prec candidates were non-leading unary: look for leading unary
        // or non-unary.  (Handles --x where all unaries have same prec.)
        for (int i = chi-1; i >= clo; --i)
            if (!v[i].unary || v[i].pos == lo) return i;
        return -1;
    } else {
        // LTR: leftmost candidate with min_prec (^ is never unary)
        for (int i = clo; i < chi; ++i)
            if (p[i] == min_prec) return i;
        return -1;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Variant 2 — direct-mp-simd
// Direct eval + AVX2 findSplit (no sparse table) + full token scan.
// ═══════════════════════════════════════════════════════════════════════════
class DirectMpSimd final : public LeanBase, public IEvaluator {
public:
    const char* name() const override { return "direct-mp-simd"; }
    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        buildAll_full();
        const uint32_t n = (uint32_t)(tokens_.size() - 1);
        int clo = 0, chi = cands_.empty() ? 0 : (int)cands_[0].size();
        return evalRange(0, n, 0, clo, chi);
    }
private:
    [[gnu::target("avx2")]]
    double evalRange(uint32_t lo, uint32_t hi, int d, int clo, int chi) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        if (clo < chi) {
            auto& v = cands_[d];
            int cp = v[clo].prec; bool cr = v[clo].rightAssoc;
            bool flat = true;
            for (int i = clo; i < chi; ++i)
                if (v[i].unary || v[i].prec != cp) { flat = false; break; }
            if (flat) {
                if (!cr) {
                    double acc = evalRange(lo, v[clo].pos, d, chi, chi);
                    for (int i = clo; i < chi; ++i) {
                        uint32_t nhi = (i+1<chi) ? v[i+1].pos : hi;
                        acc = applyBinary(tokens_[v[i].pos].type, acc,
                                          evalRange(v[i].pos+1, nhi, d, chi, chi));
                    }
                    return acc;
                } else {
                    double acc = evalRange(v[chi-1].pos+1, hi, d, chi, chi);
                    for (int i = chi-1; i >= clo; --i) {
                        uint32_t slo = (i == clo) ? lo : v[i-1].pos+1;
                        acc = applyBinary(tokens_[v[i].pos].type,
                                          evalRange(slo, v[i].pos, d, chi, chi), acc);
                    }
                    return acc;
                }
            }
        }

        int si = simdFindSplit(cands_[d], precs_[d], clo, chi, lo);
        if (si >= 0) {
            auto& c = cands_[d][si];
            if (c.unary)
                return applyUnary(tokens_[c.pos].type,
                                  evalRange(c.pos+1, hi, d, si+1, chi));
            return applyBinary(tokens_[c.pos].type,
                               evalRange(lo, c.pos, d, clo, si),
                               evalRange(c.pos+1, hi, d, si+1, chi));
        }

        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi-1) {
            int nd = d+1; auto [nb, ne] = parenRange(lo, nd);
            return evalRange(lo+1, hi-1, nd, nb, ne);
        }
        return evalLeaf(lo, hi);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Variant 3 — direct-mp-full
// Direct eval + AVX2 findSplit + operator-only buildAll.
// ═══════════════════════════════════════════════════════════════════════════
class DirectMpFull final : public LeanBase, public IEvaluator {
public:
    const char* name() const override { return "direct-mp-full"; }
    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        buildAll_oponly();
        const uint32_t n = (uint32_t)(tokens_.size() - 1);
        int clo = 0, chi = cands_.empty() ? 0 : (int)cands_[0].size();
        return evalRange(0, n, 0, clo, chi);
    }
private:
    [[gnu::target("avx2")]]
    double evalRange(uint32_t lo, uint32_t hi, int d, int clo, int chi) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        if (clo < chi) {
            auto& v = cands_[d];
            int cp = v[clo].prec; bool cr = v[clo].rightAssoc;
            bool flat = true;
            for (int i = clo; i < chi; ++i)
                if (v[i].unary || v[i].prec != cp) { flat = false; break; }
            if (flat) {
                if (!cr) {
                    double acc = evalRange(lo, v[clo].pos, d, chi, chi);
                    for (int i = clo; i < chi; ++i) {
                        uint32_t nhi = (i+1<chi) ? v[i+1].pos : hi;
                        acc = applyBinary(tokens_[v[i].pos].type, acc,
                                          evalRange(v[i].pos+1, nhi, d, chi, chi));
                    }
                    return acc;
                } else {
                    double acc = evalRange(v[chi-1].pos+1, hi, d, chi, chi);
                    for (int i = chi-1; i >= clo; --i) {
                        uint32_t slo = (i == clo) ? lo : v[i-1].pos+1;
                        acc = applyBinary(tokens_[v[i].pos].type,
                                          evalRange(slo, v[i].pos, d, chi, chi), acc);
                    }
                    return acc;
                }
            }
        }

        int si = simdFindSplit(cands_[d], precs_[d], clo, chi, lo);
        if (si >= 0) {
            auto& c = cands_[d][si];
            if (c.unary)
                return applyUnary(tokens_[c.pos].type,
                                  evalRange(c.pos+1, hi, d, si+1, chi));
            return applyBinary(tokens_[c.pos].type,
                               evalRange(lo, c.pos, d, clo, si),
                               evalRange(c.pos+1, hi, d, si+1, chi));
        }

        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi-1) {
            int nd = d+1; auto [nb, ne] = parenRange(lo, nd);
            return evalRange(lo+1, hi-1, nd, nb, ne);
        }
        return evalLeaf(lo, hi);
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_direct_mp()      { return std::make_unique<DirectMp>(); }
std::unique_ptr<IEvaluator> make_direct_mp_simd()  { return std::make_unique<DirectMpSimd>(); }
std::unique_ptr<IEvaluator> make_direct_mp_full()  { return std::make_unique<DirectMpFull>(); }

}  // namespace mp
