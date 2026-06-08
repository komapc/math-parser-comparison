// multipass_lean.cpp — divide-and-conquer direct-eval (no AST).
//
// The arena-AST variants build nodes_ then walk it in a second O(n) pass.
// direct-mp collapses both passes: the recursion returns a double directly,
// so there is no intermediate tree and no evalNode overhead.

#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"
#include "parser/token.hpp"

#include <algorithm>
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

// ───────────────────────────────────────────── shared base ─────────────────

class LeanBase {
protected:
    std::vector<Token>               tokens_;
    std::vector<std::vector<Cand>>   cands_;
    std::vector<uint32_t>            parenMatch_;
    std::vector<uint32_t>            pcStart_;  // parenCandStart
    std::vector<uint32_t>            pcEnd_;    // parenCandEnd

    // ── buildAll: full token scan ────────────────────────────────────────────
    void buildAll_full() {
        const auto n = (uint32_t)tokens_.size();
        parenMatch_.assign(n, 0);
        pcStart_.assign(n, 0);
        pcEnd_.assign(n, 0);
        cands_.clear();
        int depth = 0;
        bool expectOperand = true;
        std::vector<uint32_t> stk;
        for (uint32_t i = 0; i < n - 1; ++i) {
            switch (tokens_[i].type) {
                case TokenType::LParen:
                    stk.push_back(i); ++depth;
                    if (depth >= (int)cands_.size()) cands_.resize(depth+1);
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
                    if (depth >= (int)cands_.size()) cands_.resize(depth+1);
                    cands_[depth].push_back(c);
                    expectOperand = true;
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


}  // namespace

std::unique_ptr<IEvaluator> make_direct_mp() { return std::make_unique<DirectMp>(); }

}  // namespace mp
