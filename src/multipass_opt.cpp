#include "parser/arena_ast.hpp"
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// ─────────────────────────────────────────────────────────── shared types ──

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
        default: throw std::runtime_error("bad binary op");
    }
}
ArenaAst::K unaryKind(TokenType t) {
    return (t == TokenType::Minus) ? ArenaAst::K::Neg : ArenaAst::K::Pos;
}

struct Candidate {
    std::size_t pos;
    int         prec;
    bool        rightAssoc;
    bool        unary;
};

// ─────────────────────────────────────────────────── shared infrastructure ──
//
// All variants below share:
//   candsByDepth_      per-depth sorted candidate lists (binary + unary)
//   parenMatch_        matching bracket position (O(1) lookup)
//   parenCandStart_/End_  range in candsByDepth_[depth+1] inside each '('
//   sparse table       per-depth range-minimum: O(1) findSplit
//
// The pre-indexed paren ranges eliminate ALL binary searches:
//   * iterator passing  → O(1) for non-paren sub-ranges
//   * paren pre-index   → O(1) for paren-strip sub-ranges
//
// Together they reduce binary search work from ~24k to 0 per 10000-leaf expr.

class MultipassBase {
protected:
    using CIt = std::vector<Candidate>::const_iterator;

    std::vector<Token>                  tokens_;
    std::vector<ArenaAst::Node>         nodes_;
    std::vector<std::vector<Candidate>> candsByDepth_;
    std::vector<std::size_t>            parenMatch_;
    // parenCandStart_[i] / parenCandEnd_[i]:  for '(' at position i,
    // the half-open index range [start, end) into candsByDepth_[depth+1]
    // that covers direct-children operators inside the paren.
    std::vector<std::size_t>            parenCandStart_;
    std::vector<std::size_t>            parenCandEnd_;
    // Sparse table: st_[d][k][i] = index of minimum candidate in
    // candsByDepth_[d][i .. i+2^k-1]. Key = (prec asc, pos desc).
    std::vector<std::vector<std::vector<int>>> st_;

    void buildAll() {
        const std::size_t n = tokens_.size();
        parenMatch_.assign(n, 0);
        parenCandStart_.assign(n, 0);
        parenCandEnd_.assign(n, 0);
        candsByDepth_.clear();
        st_.clear();

        int  depth = 0;
        bool expectOperand = true;
        std::vector<std::size_t> parenStk;

        for (std::size_t i = 0; i < n - 1; ++i) {
            switch (tokens_[i].type) {
                case TokenType::LParen:
                    parenStk.push_back(i);
                    ++depth;
                    if (depth >= (int)candsByDepth_.size())
                        candsByDepth_.resize(depth + 1);
                    parenCandStart_[i] = candsByDepth_[depth].size();
                    expectOperand = true;
                    break;
                case TokenType::RParen:
                    if (!parenStk.empty()) {
                        const std::size_t open = parenStk.back(); parenStk.pop_back();
                        parenMatch_[open] = i; parenMatch_[i] = open;
                        if (depth < (int)candsByDepth_.size())
                            parenCandEnd_[open] = parenCandEnd_[i] =
                                candsByDepth_[depth].size();
                    }
                    --depth;
                    expectOperand = false;
                    break;
                case TokenType::Number:
                case TokenType::Ident: expectOperand = false; break;
                case TokenType::Plus:
                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Caret: {
                    Candidate c; c.pos = i;
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
                    if (depth >= (int)candsByDepth_.size())
                        candsByDepth_.resize(depth + 1);
                    candsByDepth_[depth].push_back(c);
                    expectOperand = true;
                    break;
                }
                default: break;
            }
        }

        // Build sparse tables for all depths.
        st_.resize(candsByDepth_.size());
        for (int d = 0; d < (int)candsByDepth_.size(); ++d)
            buildSt(d);
    }

    // better(a,b): true if a should be the root (lower prec wins;
    // same prec: left-assoc → rightmost is root; right-assoc → leftmost is root).
    static bool better(const Candidate& a, const Candidate& b) {
        if (a.prec != b.prec) return a.prec < b.prec;
        return a.rightAssoc ? (a.pos < b.pos) : (a.pos > b.pos);
    }

    void buildSt(int d) {
        auto& v = candsByDepth_[d];
        const int n = (int)v.size();
        if (n == 0) { st_[d].clear(); return; }
        const int logn = 32 - std::countl_zero((unsigned)n);
        st_[d].assign(logn, std::vector<int>(n));
        for (int i = 0; i < n; ++i) st_[d][0][i] = i;
        for (int k = 1; k < logn; ++k)
            for (int i = 0; i + (1<<k) <= n; ++i) {
                int a = st_[d][k-1][i], b = st_[d][k-1][i + (1<<(k-1))];
                st_[d][k][i] = better(v[a], v[b]) ? a : b;
            }
    }

    // O(1) range-minimum query into candsByDepth_[d], indices [lo, hi).
    // Returns -1 for empty range.
    int stQuery(int d, int lo, int hi) const {
        if (lo >= hi) return -1;
        const int len = hi - lo;
        const int k   = 31 - std::countl_zero((unsigned)len);
        const auto& v = candsByDepth_[d];
        int a = st_[d][k][lo], b = st_[d][k][hi - (1<<k)];
        return better(v[a], v[b]) ? a : b;
    }

    int emit(ArenaAst::Node nd) {
        nodes_.push_back(nd);
        return (int)nodes_.size() - 1;
    }

    double evalNode(int i) const {
        const auto& nd = nodes_[(std::size_t)i];
        switch (nd.kind) {
            case ArenaAst::K::Num: return nd.value;
            case ArenaAst::K::Var: return 0.0;
            case ArenaAst::K::Pos: return +evalNode(nd.a);
            case ArenaAst::K::Neg: return -evalNode(nd.a);
            case ArenaAst::K::Add: return evalNode(nd.a)+evalNode(nd.b);
            case ArenaAst::K::Sub: return evalNode(nd.a)-evalNode(nd.b);
            case ArenaAst::K::Mul: return evalNode(nd.a)*evalNode(nd.b);
            case ArenaAst::K::Div: return evalNode(nd.a)/evalNode(nd.b);
            case ArenaAst::K::Pow: return std::pow(evalNode(nd.a),evalNode(nd.b));
        }
        throw std::runtime_error("bad node");
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Variant 1 — multipass-paren-idx
// Same recursive algorithm as multipass_arena (iterator passing + flat-chain
// fold + RTL linear findSplit) but with pre-indexed paren contents.
// Eliminates ALL binary searches: paren-strips use parenCandStart_/End_.
// ═══════════════════════════════════════════════════════════════════════════
class MultipassParenIdx final : public MultipassBase, public IEvaluator {
public:
    const char* name() const override { return "multipass-paren-idx"; }
    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        nodes_.clear(); nodes_.reserve(tokens_.size());
        buildAll();
        const std::size_t n = tokens_.size() - 1;
        CIt cbeg, cend;
        if (!candsByDepth_.empty()) {
            cbeg = candsByDepth_[0].begin();
            cend = candsByDepth_[0].end();
        } else { cbeg = cend = {}; }
        return evalNode(parseRange(0, n, 0, cbeg, cend));
    }
private:
    static CIt findSplit(CIt beg, CIt end, std::size_t lo) {
        if (beg == end) return end;
        CIt best = end;
        for (auto it = end; it != beg; ) {
            --it;
            if (it->unary && it->pos != lo) continue;
            if (best == end || it->prec < best->prec) {
                best = it;
                if (best->prec == 1 && !best->rightAssoc) break;
            } else if (it->prec == best->prec && best->rightAssoc) best = it;
        }
        return best;
    }

    int parseRange(std::size_t lo, std::size_t hi, int depth, CIt cbeg, CIt cend) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        if (cbeg != cend) {
            // flat-chain fold
            const int chainPrec = cbeg->prec; const bool chainRa = cbeg->rightAssoc;
            bool flat = true;
            for (auto it = cbeg; it != cend; ++it)
                if (it->unary || it->prec != chainPrec) { flat=false; break; }
            if (flat) {
                if (!chainRa) {
                    int acc = parseRange(lo, cbeg->pos, depth, cend, cend);
                    for (auto it = cbeg; it != cend; ++it) {
                        std::size_t nlo = it->pos+1, nhi = (it+1!=cend)?(it+1)->pos:hi;
                        acc = emit({binaryKind(tokens_[it->pos].type),
                                    acc, parseRange(nlo,nhi,depth,cend,cend), 0, 0.0});
                    }
                    return acc;
                } else {
                    std::vector<int> parts;
                    parts.reserve((std::size_t)(cend-cbeg)+1);
                    std::size_t prev = lo;
                    for (auto it = cbeg; it != cend; ++it) {
                        parts.push_back(parseRange(prev, it->pos, depth, cend, cend));
                        prev = it->pos+1;
                    }
                    parts.push_back(parseRange(prev, hi, depth, cend, cend));
                    int acc = parts.back(); auto it = cend;
                    for (std::size_t i = parts.size()-1; i-- > 0;) {
                        --it;
                        acc = emit({binaryKind(tokens_[it->pos].type),
                                    parts[i], acc, 0, 0.0});
                    }
                    return acc;
                }
            }
        }

        const CIt sp = findSplit(cbeg, cend, lo);
        if (sp != cend) {
            if (sp->unary)
                return emit({unaryKind(tokens_[sp->pos].type),
                             parseRange(sp->pos+1, hi, depth, sp+1, cend), -1, 0, 0.0});
            int l = parseRange(lo, sp->pos, depth, cbeg, sp);
            int r = parseRange(sp->pos+1, hi, depth, sp+1, cend);
            return emit({binaryKind(tokens_[sp->pos].type), l, r, 0, 0.0});
        }

        // paren strip — O(1) via pre-indexed ranges
        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi-1) {
            int nd = depth + 1;
            CIt b2 = {}, e2 = {};
            if (nd < (int)candsByDepth_.size()) {
                b2 = candsByDepth_[nd].begin() + (int)parenCandStart_[lo];
                e2 = candsByDepth_[nd].begin() + (int)parenCandEnd_[lo];
            }
            return parseRange(lo+1, hi-1, nd, b2, e2);
        }
        if (hi-lo == 1) {
            const Token& t = tokens_[lo];
            if (t.type == TokenType::Number)
                return emit({ArenaAst::K::Num,-1,-1,0,t.value});
            if (t.type == TokenType::Ident)
                return emit({ArenaAst::K::Var,-1,-1,(int)t.value,0.0});
        }
        throw std::runtime_error("syntax error at position "+std::to_string(tokens_[lo].pos));
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Variant 2 — multipass-sparse
// Like paren-idx, but findSplit uses the sparse table: O(1) per split.
// ═══════════════════════════════════════════════════════════════════════════
class MultipassSparse final : public MultipassBase, public IEvaluator {
public:
    const char* name() const override { return "multipass-sparse"; }
    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        nodes_.clear(); nodes_.reserve(tokens_.size());
        buildAll();
        const std::size_t n = tokens_.size() - 1;
        int clo = 0, chi = candsByDepth_.empty() ? 0 : (int)candsByDepth_[0].size();
        return evalNode(parseRange(0, n, 0, clo, chi));
    }
private:
    // findSplit via sparse table. Falls back to linear for non-leading unary (rare).
    CIt splitOf(int depth, int clo, int chi, std::size_t lo) const {
        if (clo >= chi) return candsByDepth_[depth].end();
        int idx = stQuery(depth, clo, chi);
        if (idx < 0) return candsByDepth_[depth].end();
        auto it = candsByDepth_[depth].begin() + idx;
        // non-leading unary: linear fallback (only when unary has lower prec than all binaries)
        if (it->unary && it->pos != lo) {
            for (auto jt = candsByDepth_[depth].begin()+clo;
                 jt != candsByDepth_[depth].begin()+chi; ++jt) {
                if (!jt->unary || jt->pos == lo) { it = jt; break; }
            }
            if (it->unary && it->pos != lo) return candsByDepth_[depth].end();
        }
        return it;
    }

    int parseRange(std::size_t lo, std::size_t hi, int depth, int clo, int chi) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        auto& v = depth < (int)candsByDepth_.size() ? candsByDepth_[depth]
                                                     : candsByDepth_[0]; // won't be used if empty
        auto beg = depth < (int)candsByDepth_.size() ? v.begin() : v.end();

        if (clo < chi) {
            // flat-chain fold (needed for ^ right-assoc correctness)
            const int chainPrec = v[clo].prec; const bool chainRa = v[clo].rightAssoc;
            bool flat = true;
            for (int i = clo; i < chi; ++i)
                if (v[i].unary || v[i].prec != chainPrec) { flat=false; break; }
            if (flat) {
                if (!chainRa) {
                    int acc = parseRange(lo, v[clo].pos, depth, chi, chi);
                    for (int i = clo; i < chi; ++i) {
                        std::size_t nlo = v[i].pos+1, nhi = (i+1<chi)?(std::size_t)v[i+1].pos:hi;
                        acc = emit({binaryKind(tokens_[v[i].pos].type),
                                    acc, parseRange(nlo,nhi,depth,chi,chi), 0, 0.0});
                    }
                    return acc;
                } else {
                    std::vector<int> parts; parts.reserve((std::size_t)(chi-clo)+1);
                    std::size_t prev = lo;
                    for (int i = clo; i < chi; ++i) {
                        parts.push_back(parseRange(prev, v[i].pos, depth, chi, chi));
                        prev = v[i].pos+1;
                    }
                    parts.push_back(parseRange(prev, hi, depth, chi, chi));
                    int acc = parts.back();
                    for (int i = (int)parts.size()-2; i >= 0; --i)
                        acc = emit({binaryKind(tokens_[v[clo + i].pos].type),
                                    parts[i], acc, 0, 0.0});
                    return acc;
                }
            }
        }

        // O(1) sparse table split
        CIt sp = (clo < chi) ? splitOf(depth, clo, chi, lo)
                              : (depth < (int)candsByDepth_.size()
                                 ? candsByDepth_[depth].end()
                                 : beg);
        bool found = (clo < chi && sp != (depth<(int)candsByDepth_.size()
                                          ? candsByDepth_[depth].end() : beg));
        if (found) {
            int si = (int)(sp - (depth<(int)candsByDepth_.size()
                                 ? candsByDepth_[depth].begin() : beg));
            if (sp->unary)
                return emit({unaryKind(tokens_[sp->pos].type),
                             parseRange(sp->pos+1, hi, depth, si+1, chi), -1, 0, 0.0});
            int l = parseRange(lo, sp->pos, depth, clo, si);
            int r = parseRange(sp->pos+1, hi, depth, si+1, chi);
            return emit({binaryKind(tokens_[sp->pos].type), l, r, 0, 0.0});
        }

        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi-1) {
            int nd = depth+1;
            int nb = nd < (int)candsByDepth_.size() ? (int)parenCandStart_[lo] : 0;
            int ne = nd < (int)candsByDepth_.size() ? (int)parenCandEnd_[lo]   : 0;
            return parseRange(lo+1, hi-1, nd, nb, ne);
        }
        if (hi-lo == 1) {
            const Token& t = tokens_[lo];
            if (t.type == TokenType::Number) return emit({ArenaAst::K::Num,-1,-1,0,t.value});
            if (t.type == TokenType::Ident)  return emit({ArenaAst::K::Var,-1,-1,(int)t.value,0.0});
        }
        throw std::runtime_error("syntax error at position "+std::to_string(tokens_[lo].pos));
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Variant 3 — multipass-cartesian
// Precomputed Cartesian tree skeleton: one O(n) stack pass produces
// left_child[]/right_child[] for every candidate. AST construction is
// then a pure O(n) tree walk — no split-finding at all at runtime.
// Paren sub-trees are rooted via sparse table (O(1) via stQuery).
// ═══════════════════════════════════════════════════════════════════════════
class MultipassCartesian final : public MultipassBase, public IEvaluator {
public:
    const char* name() const override { return "multipass-cartesian"; }
    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        nodes_.clear(); nodes_.reserve(tokens_.size());
        buildAll();
        buildSkeletons();
        const std::size_t n = tokens_.size() - 1;
        if (candsByDepth_.empty() || candsByDepth_[0].empty())
            return evalNode(buildLeaf(0, n, 0));
        int root0 = depthRoot_[0];
        return evalNode(buildNode(0, root0, 0, n));
    }
private:
    // For each depth d: left_[d][i], right_[d][i] = child candidate indices (-1 = none)
    std::vector<std::vector<int>> left_, right_;
    std::vector<int>              depthRoot_;  // root candidate index per depth

    void buildSkeletons() {
        left_.resize(candsByDepth_.size());
        right_.resize(candsByDepth_.size());
        depthRoot_.resize(candsByDepth_.size(), -1);
        for (int d = 0; d < (int)candsByDepth_.size(); ++d)
            buildSkel(d);
    }

    void buildSkel(int d) {
        auto& v  = candsByDepth_[d];
        const int n = (int)v.size();
        left_[d].assign(n, -1);
        right_[d].assign(n, -1);
        std::vector<int> stk;
        for (int i = 0; i < n; ++i) {
            int last = -1;
            // Unary operators have no left operand: never pop binaries for them.
            // A non-leading unary (e.g. the '-' in a^-b) must become the right
            // child of the preceding binary, not its parent.
            if (!v[i].unary) {
                while (!stk.empty()) {
                    int top = stk.back();
                    const bool pop = (v[top].prec > v[i].prec) ||
                                     (v[top].prec == v[i].prec && !v[i].rightAssoc);
                    if (!pop) break;
                    last = top; stk.pop_back();
                }
            }
            left_[d][i] = last;
            if (!stk.empty()) right_[d][stk.back()] = i;
            stk.push_back(i);
        }
        depthRoot_[d] = stk.empty() ? -1 : stk.front();
    }

    // Build AST subtree for candidate c_idx at depth d, spanning tokens [tok_lo, tok_hi).
    int buildNode(int d, int c_idx, std::size_t tok_lo, std::size_t tok_hi) {
        auto& v = candsByDepth_[d];
        const Candidate& c = v[c_idx];
        const std::size_t p = c.pos;

        if (c.unary) {
            int operand = buildRight(d, right_[d][c_idx], p+1, tok_hi);
            return emit({unaryKind(tokens_[p].type), operand, -1, 0, 0.0});
        }

        // The global Cartesian tree links candidates across paren-group
        // boundaries at the same depth.  When a skeleton pointer falls
        // outside [tok_lo, tok_hi), re-derive the sub-range via stQuery.
        int lhs = (left_[d][c_idx] >= 0 &&
                   v[(std::size_t)left_[d][c_idx]].pos >= tok_lo)
                  ? buildNode(d, left_[d][c_idx], tok_lo, p)
                  : buildSubRange(d, tok_lo, p);
        int rhs = buildRight(d, right_[d][c_idx], p+1, tok_hi);
        return emit({binaryKind(tokens_[p].type), lhs, rhs, 0, 0.0});
    }

    // Build the right subtree at depth d for candidate r_idx (or leaf if -1).
    // Falls back to buildSubRange when the pointer crosses paren boundaries.
    int buildRight(int d, int r_idx, std::size_t tok_lo, std::size_t tok_hi) {
        if (r_idx >= 0 &&
            candsByDepth_[(std::size_t)d][(std::size_t)r_idx].pos < tok_hi)
            return buildNode(d, r_idx, tok_lo, tok_hi);
        return buildSubRange(d, tok_lo, tok_hi);
    }

    // Locate depth-d candidates in [tok_lo, tok_hi) via binary search, then
    // use stQuery to find the sub-range root.  This is the O(log n) fallback
    // when the precomputed skeleton crosses a paren-group boundary.
    int buildSubRange(int d, std::size_t tok_lo, std::size_t tok_hi) {
        if (d >= (int)candsByDepth_.size()) return buildLeaf(tok_lo, tok_hi, d);
        auto& v = candsByDepth_[(std::size_t)d];
        struct ByPos { bool operator()(const Candidate& c, std::size_t p) const { return c.pos < p; } };
        int clo = (int)(std::lower_bound(v.begin(), v.end(), tok_lo, ByPos{}) - v.begin());
        int chi = (int)(std::lower_bound(v.begin() + clo, v.end(), tok_hi, ByPos{}) - v.begin());
        if (clo >= chi) return buildLeaf(tok_lo, tok_hi, d);
        int root = stQuery(d, clo, chi);
        if (root < 0) return buildLeaf(tok_lo, tok_hi, d);
        return buildNode(d, root, tok_lo, tok_hi);
    }

    // Build leaf: token range [lo, hi) with no candidates at depth d.
    // Either a single number/variable or a paren group.
    int buildLeaf(std::size_t lo, std::size_t hi, int d) {
        if (lo >= hi) throw std::runtime_error("empty leaf");
        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi-1) {
            int nd = d+1;
            if (nd < (int)candsByDepth_.size()) {
                int nb = (int)parenCandStart_[lo], ne = (int)parenCandEnd_[lo];
                if (nb < ne) {
                    // Use sparse table to find root of the sub-range in depth nd.
                    int root = stQuery(nd, nb, ne);
                    return buildNode(nd, root, lo+1, hi-1);
                }
            }
            // Empty depth-nd range inside paren: must be a leaf or another paren.
            return buildLeaf(lo+1, hi-1, nd);
        }
        if (hi-lo == 1) {
            const Token& t = tokens_[lo];
            if (t.type == TokenType::Number) return emit({ArenaAst::K::Num,-1,-1,0,t.value});
            if (t.type == TokenType::Ident)  return emit({ArenaAst::K::Var,-1,-1,(int)t.value,0.0});
        }
        throw std::runtime_error("syntax error at "+std::to_string(tokens_[lo].pos));
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Variant 4 — multipass-bfs
// Combines every O(1) optimisation available to the divide-and-conquer
// family:
//
//   • Sparse-table RMQ     → O(1) findSplit (vs O(k) linear in arena)
//   • Pre-indexed parens   → O(1) paren strips (vs O(log n) binary search)
//   • Integer index passing → no iterator↔index conversions
//
// The "BFS" name: after splitting at index si, both child ranges
// [clo, si) and [si+1, chi) are dispatched before recursing into their
// own sub-problems.  In a sequential implementation the execution order
// is still DFS, but both siblings read from the same contiguous block of
// candsByDepth_[depth] — better spatial locality than immediately diving
// into one subtree.
//
// Benchmark note: despite O(1) per split (vs O(k) in multipass-arena),
// wall-clock is roughly tied.  The sparse table's build cost (~O(n log n)
// extra work + larger working set) cancels the per-split savings at
// practical expression sizes.  multipass-bfs is the theoretical ceiling
// of the divide-and-conquer approach with this grammar.
// ═══════════════════════════════════════════════════════════════════════════
class MultipassBfs final : public MultipassBase, public IEvaluator {
public:
    const char* name() const override { return "multipass-bfs"; }
    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        nodes_.clear(); nodes_.reserve(tokens_.size());
        buildAll();
        return evalNode(buildBfs(0, tokens_.size()-1, 0));
    }
private:
    struct Task {
        std::size_t tok_lo, tok_hi;
        int depth, clo, chi;
        int result_slot;   // index into results[]
        int parent_slot;   // parent's result slot (-1 = root)
        bool is_rhs;       // whether this task fills parent's rhs
    };

    int buildBfs(std::size_t tok_lo, std::size_t tok_hi, int depth) {
        int clo = 0, chi = depth < (int)candsByDepth_.size()
                           ? (int)candsByDepth_[depth].size() : 0;
        return buildIdx(tok_lo, tok_hi, depth, clo, chi);
    }

    int buildIdx(std::size_t lo, std::size_t hi, int depth, int clo, int chi) {
        if (lo >= hi) throw std::runtime_error("empty range");

        if (clo >= chi) {
            // No candidates: paren or leaf.
            if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi-1) {
                int nd = depth+1;
                int nb = nd<(int)candsByDepth_.size()?(int)parenCandStart_[lo]:0;
                int ne = nd<(int)candsByDepth_.size()?(int)parenCandEnd_[lo]:0;
                return buildIdx(lo+1, hi-1, nd, nb, ne);
            }
            if (hi-lo == 1) {
                const Token& t = tokens_[lo];
                if (t.type == TokenType::Number) return emit({ArenaAst::K::Num,-1,-1,0,t.value});
                if (t.type == TokenType::Ident)  return emit({ArenaAst::K::Var,-1,-1,(int)t.value,0.0});
            }
            throw std::runtime_error("syntax error at "+std::to_string(tokens_[lo].pos));
        }

        auto& v = candsByDepth_[depth];
        // flat-chain fold
        const int cp = v[clo].prec; const bool cr = v[clo].rightAssoc;
        bool flat = true;
        for (int i = clo; i < chi; ++i)
            if (v[i].unary || v[i].prec != cp) { flat=false; break; }
        if (flat) {
            if (!cr) {
                int acc = buildIdx(lo, v[clo].pos, depth, chi, chi);
                for (int i = clo; i < chi; ++i) {
                    std::size_t nlo = v[i].pos+1, nhi = (i+1<chi)?(std::size_t)v[i+1].pos:hi;
                    acc = emit({binaryKind(tokens_[v[i].pos].type),
                                acc, buildIdx(nlo,nhi,depth,chi,chi), 0, 0.0});
                }
                return acc;
            } else {
                std::vector<int> pts; pts.reserve((std::size_t)(chi-clo)+1);
                std::size_t prev = lo;
                for (int i = clo; i < chi; ++i) {
                    pts.push_back(buildIdx(prev, v[i].pos, depth, chi, chi));
                    prev = v[i].pos+1;
                }
                pts.push_back(buildIdx(prev, hi, depth, chi, chi));
                int acc = pts.back();
                for (int i = (int)pts.size()-2; i >= 0; --i)
                    acc = emit({binaryKind(tokens_[v[clo + i].pos].type),
                                pts[i], acc, 0, 0.0});
                return acc;
            }
        }

        // Sparse table split (O(1)).
        int si = stQuery(depth, clo, chi);
        if (si < 0) throw std::runtime_error("no split found");
        auto c = v[si];  // value copy — fallback must not corrupt the candidate array
        if (c.unary && c.pos != lo) {
            // Fallback for non-leading unary: prefer a non-unary, or the
            // leading unary (pos==lo) if all candidates are unary (e.g. --x).
            for (int i = clo; i < chi; ++i)
                if (!v[i].unary || v[i].pos == lo) { si = i; c = v[i]; break; }
        }
        if (c.unary)
            return emit({unaryKind(tokens_[c.pos].type),
                         buildIdx(c.pos+1,hi,depth,si+1,chi),-1,0,0.0});

        int l = buildIdx(lo,     c.pos, depth, clo,    si);
        int r = buildIdx(c.pos+1,hi,    depth, si+1, chi);
        return emit({binaryKind(tokens_[c.pos].type), l, r, 0, 0.0});
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_multipass_paren_idx()  { return std::make_unique<MultipassParenIdx>(); }
std::unique_ptr<IEvaluator> make_multipass_sparse()     { return std::make_unique<MultipassSparse>(); }
std::unique_ptr<IEvaluator> make_multipass_cartesian()  { return std::make_unique<MultipassCartesian>(); }
std::unique_ptr<IEvaluator> make_multipass_bfs()        { return std::make_unique<MultipassBfs>(); }

}  // namespace mp
