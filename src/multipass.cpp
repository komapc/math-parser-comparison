#include "parser/lexer.hpp"
#include "parser/parser.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// In one sentence: recursively find the lowest-precedence operator at paren-depth
// zero, make it the tree's root, and recurse on the two halves to either side.
//
// "Divide-and-conquer" / recursive-split parsing.
//
// Fixes applied vs. the naive O(n^2) version:
//   #1  Pre-scan candidates: one O(n) pass collects every operator at every
//       paren-nesting depth into per-depth sorted lists. parseRange(lo,hi,depth)
//       binary-searches the right list instead of re-scanning the full token
//       array, so each split costs O(k) where k = operators in that sub-range.
//       Average complexity drops to O(n log n).
//   #5  Right-to-left scan with early exit: scanning candidates RTL gives
//       left-associative tie-breaking (rightmost wins) for free, and we stop
//       as soon as prec==1 (the minimum possible) is found.
//   #6  Flat-chain folding: when every candidate in a range shares one
//       precedence level, build the sub-tree iteratively (left- or right-fold)
//       instead of recursing O(n) deep — eliminates the O(n^2) worst case on
//       skewed chains like 1+2+3+...+n.
//
// The nesting depth is threaded through parseRange() so the correct per-depth
// candidate list is consulted at every level of recursion, including inside
// stripped parentheses.
//
// Precedence:
//   binary + -  : 1   left-assoc  -> split on the RIGHTMOST
//   binary * /  : 2   left-assoc  -> split on the RIGHTMOST
//   unary  + -  : 3   prefix; only valid as root when it leads the range
//   ^           : 4   right-assoc -> split on the LEFTMOST

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
    bool        unary;      // only valid as root when pos == lo of the range
};

class MultiPass final : public IParser {
public:
    const char* name() const override { return "multipass"; }

    ExprPtr parse(std::string_view src) override {
        tokens_ = tokenize(src);
        buildCandidates();
        return parseRange(0, tokens_.size() - 1, 0);
    }

private:
    std::vector<Token>              tokens_;
    // candsByDepth_[d] = operators at nesting depth d, sorted by position.
    std::vector<std::vector<Candidate>> candsByDepth_;

    // Fix #1: single O(n) pass — record every operator at its actual nesting depth.
    void buildCandidates() {
        candsByDepth_.clear();
        int  depth = 0;
        bool expectOperand = true;
        const std::size_t n = tokens_.size() - 1;  // exclude End sentinel
        for (std::size_t i = 0; i < n; ++i) {
            switch (tokens_[i].type) {
                case TokenType::LParen: ++depth; expectOperand = true;  break;
                case TokenType::RParen: --depth; expectOperand = false; break;
                case TokenType::Number:
                case TokenType::Ident:  expectOperand = false; break;
                case TokenType::Plus:
                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Caret: {
                    Candidate c;
                    c.pos = i;
                    if (expectOperand) {
                        // prefix unary: +/-
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

    using CIt = std::vector<Candidate>::const_iterator;

    // Binary-search into the per-depth list for token range [lo, hi).
    std::pair<CIt, CIt> candRange(std::size_t lo, std::size_t hi, int depth) const {
        static const std::vector<Candidate> empty;
        const auto& v = (depth >= 0 && depth < static_cast<int>(candsByDepth_.size()))
                        ? candsByDepth_[static_cast<std::size_t>(depth)] : empty;
        auto b = std::lower_bound(v.begin(), v.end(), lo, ByPos{});
        auto e = std::lower_bound(b,         v.end(), hi, ByPos{});
        return {b, e};
    }

    // Fix #5: scan RTL for left-assoc tie-break (rightmost wins = first found RTL).
    // Early exit the moment prec==1 is found. Right-assoc (^): override with the
    // leftmost by continuing to scan; can't early-exit in that case.
    const Candidate* findSplit(CIt beg, CIt end, std::size_t lo) const {
        if (beg == end) return nullptr;
        const Candidate* best = nullptr;
        for (auto it = end; it != beg; ) {
            --it;
            if (it->unary && it->pos != lo) continue;
            if (!best || it->prec < best->prec) {
                best = &*it;
                if (best->prec == 1 && !best->rightAssoc) break;  // fix #5: early exit
            } else if (it->prec == best->prec && best->rightAssoc) {
                best = &*it;  // right-assoc: prefer leftmost
            }
        }
        return best;
    }

    ExprPtr parseRange(std::size_t lo, std::size_t hi, int depth) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        auto [cbeg, cend] = candRange(lo, hi, depth);

        // Fix #6: flat-chain detection.
        // All candidates share one precedence -> build iteratively.
        if (cbeg != cend) {
            const int  chainPrec = cbeg->prec;
            const bool chainRa   = cbeg->rightAssoc;
            bool flat = true;
            for (auto it = cbeg; it != cend; ++it)
                if (it->unary || it->prec != chainPrec) { flat = false; break; }

            if (flat) {
                if (!chainRa) {
                    // Left fold: ((a op b) op c) op d ...
                    ExprPtr acc = parseRange(lo, cbeg->pos, depth);
                    for (auto it = cbeg; it != cend; ++it) {
                        const std::size_t nextLo = it->pos + 1;
                        const std::size_t nextHi = (it + 1 != cend) ? (it + 1)->pos : hi;
                        acc = binary(tokens_[it->pos].type,
                                     std::move(acc), parseRange(nextLo, nextHi, depth));
                    }
                    return acc;
                } else {
                    // Right fold: a op (b op (c op d)) ...
                    std::vector<ExprPtr> parts;
                    parts.reserve(static_cast<std::size_t>(cend - cbeg) + 1);
                    std::size_t prevLo = lo;
                    for (auto it = cbeg; it != cend; ++it) {
                        parts.push_back(parseRange(prevLo, it->pos, depth));
                        prevLo = it->pos + 1;
                    }
                    parts.push_back(parseRange(prevLo, hi, depth));
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

        // General case: find the one split point.
        if (const Candidate* split = findSplit(cbeg, cend, lo)) {
            if (split->unary)
                return unary(tokens_[split->pos].type,
                             parseRange(split->pos + 1, hi, depth));
            return binary(tokens_[split->pos].type,
                          parseRange(lo, split->pos, depth),
                          parseRange(split->pos + 1, hi, depth));
        }

        // No depth-d operator: parenthesized group or single primary.
        if (tokens_[lo].type == TokenType::LParen) {
            // Verify '(' at lo closes at hi-1 (guards against e.g. (a)*(b)).
            int d = 0;
            std::size_t k = lo;
            for (; k < hi; ++k) {
                if      (tokens_[k].type == TokenType::LParen) ++d;
                else if (tokens_[k].type == TokenType::RParen && --d == 0) break;
            }
            if (k == hi - 1)
                return parseRange(lo + 1, hi - 1, depth + 1);
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
