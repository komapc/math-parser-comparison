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
//   #7  Bounded scans + precedence buckets: #5 and #6 are still O(k) linear
//       scans when their early exits never fire (mixed-precedence chains with
//       no + or -; a long flat prefix rescanned before every split — see
//       bench/adversarial_bench.cpp). Both scans now give up after
//       kScanBudget candidates and answer from per-depth sorted position
//       arrays (one per precedence class) by binary search: worst case drops
//       from Theta(n^2) to O(n log n).
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

// Fix #7: sorted token positions of one depth's candidates, split by
// precedence class — the O(log n) fallback once a linear scan exceeds its
// budget.
struct Buckets {
    std::vector<std::size_t> p1, p2, caret, un;
};

constexpr std::ptrdiff_t kScanBudget = 16;
constexpr std::size_t    kNone       = static_cast<std::size_t>(-1);

// Rightmost position in sorted `v` within [lo, hi), or kNone.
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
    std::vector<std::vector<Candidate>> candsByDepth_;
    std::vector<Buckets>            bucketsByDepth_;  // fix #7
    std::vector<std::size_t>        parenMatch_;  // parenMatch_[i] = index of matching bracket

    // Single O(n) pass: record operators at every nesting depth AND precompute
    // matching-paren indices so paren-stripping in parseRange() is O(1) not O(n).
    void buildCandidates() {
        // Clear contents but keep inner-vector capacities (no realloc churn).
        for (auto& v : candsByDepth_) v.clear();
        for (auto& b : bucketsByDepth_) {
            b.p1.clear(); b.p2.clear(); b.caret.clear(); b.un.clear();
        }
        const std::size_t n = tokens_.size();
        parenMatch_.assign(n, 0);

        int  depth = 0;
        bool expectOperand = true;
        std::vector<std::size_t> stack;  // open-paren positions for matching

        for (std::size_t i = 0; i < n - 1; ++i) {  // exclude End sentinel
            switch (tokens_[i].type) {
                case TokenType::LParen:
                    stack.push_back(i);
                    ++depth; expectOperand = true;
                    break;
                case TokenType::RParen:
                    if (!stack.empty()) {
                        std::size_t open = stack.back(); stack.pop_back();
                        parenMatch_[open] = i;
                        parenMatch_[i]    = open;
                    }
                    --depth; expectOperand = false;
                    break;
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
                    }
                    candsByDepth_[static_cast<std::size_t>(depth)].push_back(c);
                    auto& bk = bucketsByDepth_[static_cast<std::size_t>(depth)];
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

    // Fix #7: bucket-based equivalent of the full RTL scan, O(log n) — the
    // rightmost prec-1 candidate, else the rightmost prec-2, else the range's
    // first candidate (a leading unary, or the leftmost right-assoc ^; a unary
    // not at the range start can't be first, since the token before it is a
    // same-depth operator, which would itself be the earlier candidate).
    const Candidate* findSplitBuckets(CIt beg, CIt end, int depth) const {
        const auto& bk = bucketsByDepth_[static_cast<std::size_t>(depth)];
        const std::size_t blo = beg->pos, bhi = end[-1].pos + 1;
        std::size_t p = rightmostIn(bk.p1, blo, bhi);
        if (p == kNone) p = rightmostIn(bk.p2, blo, bhi);
        if (p == kNone) return &*beg;
        return &*std::lower_bound(beg, end, p, ByPos{});
    }

    // Fix #7: flatness answered from the buckets — true iff every candidate
    // in [beg, end) is binary with one precedence. O(log n), never O(k).
    bool flatByBuckets(CIt beg, CIt end, int depth) const {
        const auto& bk = bucketsByDepth_[static_cast<std::size_t>(depth)];
        const std::size_t blo = beg->pos, bhi = end[-1].pos + 1;
        if (anyIn(bk.un, blo, bhi)) return false;
        const int classes = (anyIn(bk.p1, blo, bhi) ? 1 : 0) +
                            (anyIn(bk.p2, blo, bhi) ? 1 : 0) +
                            (anyIn(bk.caret, blo, bhi) ? 1 : 0);
        return classes == 1;
    }

    // Fix #5: scan RTL for left-assoc tie-break (rightmost wins = first found RTL).
    // Early exit the moment prec==1 is found. Right-assoc (^): override with the
    // leftmost by continuing to scan; can't early-exit in that case.
    // Fix #7: the scan is bounded — past kScanBudget candidates without a
    // prec-1 hit, the answer comes from the buckets instead.
    const Candidate* findSplit(CIt beg, CIt end, std::size_t lo, int depth) const {
        if (beg == end) return nullptr;
        const Candidate* best = nullptr;
        const CIt stop = (end - beg > kScanBudget) ? end - kScanBudget : beg;
        for (auto it = end; it != stop; ) {
            --it;
            if (it->unary && it->pos != lo) continue;
            if (!best || it->prec < best->prec) {
                best = &*it;
                if (best->prec == 1 && !best->rightAssoc) return best;  // fix #5
            } else if (it->prec == best->prec && best->rightAssoc) {
                best = &*it;  // right-assoc: prefer leftmost
            }
        }
        if (stop == beg) return best;     // scanned everything: exact answer
        return findSplitBuckets(beg, end, depth);
    }

    ExprPtr parseRange(std::size_t lo, std::size_t hi, int depth) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        auto [cbeg, cend] = candRange(lo, hi, depth);

        // Fix #6: flat-chain detection.
        // All candidates share one precedence -> build iteratively.
        if (cbeg != cend) {
            const int  chainPrec = cbeg->prec;
            const bool chainRa   = cbeg->rightAssoc;
            // Fix #7 (hybrid): scan a bounded prefix — a mixed range is
            // usually disproved within a few candidates (old cost); only a
            // uniform prefix longer than the budget asks the buckets to vouch
            // for the rest (O(log n), never an O(k) rescan).
            bool flat = true;
            const CIt scanEnd =
                (cend - cbeg > kScanBudget) ? cbeg + kScanBudget : cend;
            for (auto it = cbeg; it != scanEnd; ++it)
                if (it->unary || it->prec != chainPrec) { flat = false; break; }
            if (flat && scanEnd != cend)
                flat = flatByBuckets(cbeg, cend, depth);

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
        if (const Candidate* split = findSplit(cbeg, cend, lo, depth)) {
            if (split->unary)
                return unary(tokens_[split->pos].type,
                             parseRange(split->pos + 1, hi, depth));
            return binary(tokens_[split->pos].type,
                          parseRange(lo, split->pos, depth),
                          parseRange(split->pos + 1, hi, depth));
        }

        // No depth-d operator: parenthesized group or single primary.
        if (tokens_[lo].type == TokenType::LParen &&
            parenMatch_[lo] == hi - 1)              // O(1) — precomputed
            return parseRange(lo + 1, hi - 1, depth + 1);
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
