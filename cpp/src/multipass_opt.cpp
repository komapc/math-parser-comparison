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

// Sorted token positions of one depth's candidates, split by precedence class.
// The sparse table makes findSplit O(1), but the flat-chain *check* was still
// an O(k) scan — quadratic on a long same-precedence prefix rescanned before
// every split (towerchain in bench/adversarial_bench.cpp). Past kScanBudget
// candidates the check is answered from these buckets in O(log n) instead.
struct Buckets {
    std::vector<uint32_t> p1, p2, caret, un;
};

constexpr int kScanBudget = 16;

static bool anyIn(const std::vector<uint32_t>& v, uint32_t lo, uint32_t hi) {
    auto it = std::lower_bound(v.begin(), v.end(), lo);
    return it != v.end() && *it < hi;
}

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
    std::vector<Buckets>                bucketsByDepth_;
    const double*                       vars_ = nullptr;
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
        // Clear contents but keep inner-vector capacities (no realloc churn).
        for (auto& v : candsByDepth_) v.clear();
        for (auto& b : bucketsByDepth_) {
            b.p1.clear(); b.p2.clear(); b.caret.clear(); b.un.clear();
        }
        st_.clear();

        int  depth = 0;
        bool expectOperand = true;
        std::vector<std::size_t> parenStk;

        for (std::size_t i = 0; i < n - 1; ++i) {
            switch (tokens_[i].type) {
                case TokenType::LParen:
                    parenStk.push_back(i);
                    ++depth;
                    if (depth >= (int)candsByDepth_.size()) {
                        candsByDepth_.resize(depth + 1);
                        bucketsByDepth_.resize(depth + 1);
                    }
                    parenCandStart_[i] = candsByDepth_[depth].size();
                    expectOperand = true;
                    break;
                case TokenType::RParen: {
                    // a stray ')' would drive depth negative and index the
                    // per-depth candidate buckets out of bounds
                    if (parenStk.empty()) throw std::runtime_error("mismatched parenthesis");
                    const std::size_t open = parenStk.back(); parenStk.pop_back();
                    parenMatch_[open] = i; parenMatch_[i] = open;
                    if (depth < (int)candsByDepth_.size())
                        parenCandEnd_[open] = parenCandEnd_[i] =
                            candsByDepth_[depth].size();
                    --depth;
                    expectOperand = false;
                    break;
                }
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
                    if (depth >= (int)candsByDepth_.size()) {
                        candsByDepth_.resize(depth + 1);
                        bucketsByDepth_.resize(depth + 1);
                    }
                    candsByDepth_[depth].push_back(c);
                    auto& bk = bucketsByDepth_[depth];
                    const auto pos32 = (uint32_t)i;
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
            case ArenaAst::K::Var: return vars_ ? vars_[nd.var] : 0.0;
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
class MultipassBfs final : public MultipassBase, public IEvaluator {
public:
    const char* name() const override { return "multipass-bfs"; }
    double eval(std::string_view src, const double* vars = nullptr) override {
        tokens_ = tokenize(src);
        vars_ = vars;
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
        // flat-chain fold — hybrid bounded check: scan a prefix (mixed ranges
        // are usually disproved within a few candidates); only a uniform
        // prefix longer than the budget asks the buckets for the rest
        // (O(log n), never an O(k) rescan of a long flat run).
        const int cp = v[clo].prec; const bool cr = v[clo].rightAssoc;
        bool flat = true;
        const int scanEnd = (chi - clo > kScanBudget) ? clo + kScanBudget : chi;
        for (int i = clo; i < scanEnd; ++i)
            if (v[i].unary || v[i].prec != cp) { flat=false; break; }
        if (flat && scanEnd != chi) {
            const auto& bk = bucketsByDepth_[depth];
            const uint32_t blo = (uint32_t)v[clo].pos,
                           bhi = (uint32_t)v[chi-1].pos + 1;
            flat = !anyIn(bk.un, blo, bhi) &&
                   ((anyIn(bk.p1, blo, bhi) ? 1 : 0) +
                    (anyIn(bk.p2, blo, bhi) ? 1 : 0) +
                    (anyIn(bk.caret, blo, bhi) ? 1 : 0)) == 1;
        }
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

std::unique_ptr<IEvaluator> make_multipass_bfs()        { return std::make_unique<MultipassBfs>(); }

}  // namespace mp
