// Parallel multipass arena, take two: persistent pool + atomic-free arena, plus
// a direct-eval (tree-free) sibling.
//
// Same split/tree logic as multipass_parallel.cpp (middle-split, flat-chain
// bisection, fork-join at the top log2(W) levels). Three changes make the
// fork-join actually pay, shared by both variants below:
//
//   1. Persistent thread pool (help-while-waiting) instead of std::async.
//      std::async(launch::async) spawns a fresh OS thread per fork (~10-50us);
//      for a subtree that parses in ~1us that is pure loss, which is why the
//      shipped par2/4/8 are usually SLOWER than par1. Here W-1 workers are
//      created once; a fork enqueues the rhs and the parent, after finishing
//      lhs, drains the queue itself (helpUntil) rather than blocking a core.
//      Draining *any* pending task while waiting also makes deadlock impossible
//      with a fixed pool.
//
//   2. Atomic-free node allocation (tree variant). Every node corresponds to
//      exactly one token, and the recursion partitions the token range, so
//      disjoint sub-parses own disjoint node slots. Storing each node at its
//      owner-token index removes the shared std::atomic<int> nextNode_ and its
//      cross-core cache-line bouncing.
//
//   3. Tuned fork threshold (~128 tokens, env-overridable). The std::async
//      variant needed a high threshold to avoid thread-spawn per fork; with the
//      pool that hand-off is cheap, so a *low* threshold pays — it feeds all
//      workers instead of starving them. Swept: 2048 starves, ~128 is the knee.
//
// The direct-eval variant (MultipassDirectFork) additionally fuses evaluation
// into the parallel recursion: parseRange returns a double, so there is no tree
// to walk afterwards and no node array at all — an attempt to pull evalNode out
// of the serial fraction that caps the tree-building variant.
#include "parser/arena_ast.hpp"
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mp {
namespace {

// ---- grammar helpers (mirror multipass_parallel.cpp) -----------------------
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

double applyBin(TokenType t, double a, double b) {
    switch (t) {
        case TokenType::Plus:  return a + b;
        case TokenType::Minus: return a - b;
        case TokenType::Star:  return a * b;
        case TokenType::Slash: return a / b;
        case TokenType::Caret: return std::pow(a, b);
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

// ---- persistent fork-join pool ---------------------------------------------
// Workers sleep on the condition variable when idle; a parent waiting on a
// forked child calls helpUntil() and runs queued tasks in the meantime, so no
// core sits idle and a fixed-size pool never deadlocks (there is always a
// runnable task or the awaited flag is already set).
class ForkPool {
public:
    explicit ForkPool(unsigned workers) {
        threads_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i)
            threads_.emplace_back([this] { workerLoop(); });
    }
    ~ForkPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }

    void submit(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    void helpUntil(const std::atomic<bool>& done) {
        for (;;) {
            std::function<void()> job;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (!q_.empty()) {
                    job = std::move(q_.front());
                    q_.pop_front();
                }
            }
            if (job) { job(); continue; }
            if (done.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();
        }
    }

    std::vector<std::thread>          threads_;
    std::deque<std::function<void()>> q_;
    std::mutex                        m_;
    std::condition_variable           cv_;
    bool                              stop_ = false;
};

// ---- shared read-only machinery (candidate index + split finding) ----------
// tokens_, candsByDepth_, parenMatch_ are written only in buildCandidates()
// (single-threaded) and read-only during all parse calls.
class MpForkCore {
protected:
    using CIt = std::vector<Candidate>::const_iterator;

    // Fork only for ranges above this many tokens. Env-tunable (MP_FORK_THRESHOLD)
    // so the granularity can be swept without recompiling.
    const std::size_t forkThreshold_;

    const int   maxForkDepth_;
    ForkPool    pool_;
    const double* vars_ = nullptr;

    static std::size_t envThreshold() {
        if (const char* s = std::getenv("MP_FORK_THRESHOLD")) {
            const unsigned long long v = std::strtoull(s, nullptr, 10);
            if (v > 0) return static_cast<std::size_t>(v);
        }
        // ~128 tokens is the swept sweet spot: fine enough to feed 8 workers,
        // coarse enough that the pool hand-off amortises. (A high threshold only
        // made sense for the std::async variant, where each fork cost a thread.)
        return 128;
    }

    std::vector<Token>                  tokens_;
    std::vector<std::vector<Candidate>> candsByDepth_;
    std::vector<std::size_t>            parenMatch_;

    // workers = 2^maxForkDepth - 1: the main thread is the +1, so peak
    // concurrency during a parse is 2^maxForkDepth = the advertised width.
    explicit MpForkCore(int maxForkDepth)
        : forkThreshold_(envThreshold()),
          maxForkDepth_(maxForkDepth), pool_((1u << maxForkDepth) - 1u) {}

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
                case TokenType::RParen: {
                    if (stack.empty()) throw std::runtime_error("mismatched parenthesis");
                    const std::size_t open = stack.back(); stack.pop_back();
                    parenMatch_[open] = i; parenMatch_[i] = open;
                    --depth; expectOperand = false; break;
                }
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

    CIt findSplit(CIt beg, CIt end, std::size_t lo, std::size_t hi) const {
        if (beg == end) return end;
        int minPrec = std::numeric_limits<int>::max();
        for (auto it = beg; it != end; ++it)
            if (!it->unary || it->pos == lo)
                minPrec = std::min(minPrec, it->prec);
        if (minPrec == std::numeric_limits<int>::max()) return end;

        const std::size_t mid = (lo + hi) / 2;
        CIt   best     = end;
        std::size_t bestDist = std::numeric_limits<std::size_t>::max();
        for (auto it = beg; it != end; ++it) {
            if (it->unary) continue;
            if (it->prec != minPrec || it->rightAssoc) continue;
            const auto tt = tokens_[it->pos].type;
            if (tt != TokenType::Plus && tt != TokenType::Star) continue;
            const std::size_t d = (it->pos >= mid) ? it->pos - mid : mid - it->pos;
            if (d < bestDist) { bestDist = d; best = it; }
        }
        if (best != end) return best;

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

    // Fork rhsFn onto the pool, run lhsFn inline, join. Exception-safe in both
    // directions: a throw inside the task is captured and rethrown here (never
    // on a pool thread, which would std::terminate); a throw from lhsFn still
    // waits for the task, because the task writes into this frame's locals.
    template <class T, class RhsFn, class LhsFn>
    std::pair<T, T> forkJoinImpl(RhsFn&& rhsFn, LhsFn&& lhsFn) {
        T rhs{};
        std::atomic<bool> done{false};
        std::exception_ptr err;
        pool_.submit([&] {
            try { rhs = rhsFn(); } catch (...) { err = std::current_exception(); }
            done.store(true, std::memory_order_release);
        });
        T lhs{};
        try {
            lhs = lhsFn();
        } catch (...) {
            pool_.helpUntil(done);
            throw;
        }
        pool_.helpUntil(done);
        if (err) std::rethrow_exception(err);
        return {lhs, rhs};
    }

    // tokenize + candidate index + top-level candidate range. Shared prologue.
    struct Setup { std::size_t n; CIt cbeg; CIt cend; };
    Setup setup(std::string_view src) {
        tokens_ = tokenize(src);
        buildCandidates();
        const std::size_t n = tokens_.size() - 1;
        auto [cbeg, cend] = candRange(0, n, 0);
        return {n, cbeg, cend};
    }
};

// ---- tree-building variant: fork-join into an arena, atomic-free -----------
class MultipassPool final : public IEvaluator, MpForkCore {
    const char* name_;
    std::vector<ArenaAst::Node> nodes_;   // node lives at its owner-token index

    int emitAt(std::size_t ownerPos, ArenaAst::Node nd) {
        nodes_[ownerPos] = nd;
        return static_cast<int>(ownerPos);
    }

    int forkJoin(std::size_t rlo, std::size_t rhi, int depth, CIt rbeg, CIt rend,
                 int fd, std::size_t llo, std::size_t lhi, CIt lbeg, CIt lend,
                 std::size_t ownerPos, TokenType opType) {
        const auto [lhs, rhs] = forkJoinImpl<int>(
            [&] { return parseRange(rlo, rhi, depth, rbeg, rend, fd); },
            [&] { return parseRange(llo, lhi, depth, lbeg, lend, fd); });
        return emitAt(ownerPos, {binaryKind(opType), lhs, rhs, 0, 0.0});
    }

    int parseRange(std::size_t lo, std::size_t hi, int depth,
                   CIt cbeg, CIt cend, int forkDepth) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        if (cbeg != cend) {
            const int  chainPrec = cbeg->prec;
            const bool chainRa   = cbeg->rightAssoc;
            bool flat = true;
            for (auto it = cbeg; it != cend; ++it)
                if (it->unary || it->prec != chainPrec) { flat = false; break; }

            if (flat) {
                if (!chainRa) {
                    const CIt splitAt = findChainBisect(cbeg, cend, lo, hi);
                    if (splitAt != cend) {
                        if (forkDepth > 0 && hi - lo > forkThreshold_) {
                            return forkJoin(splitAt->pos + 1, hi, depth, splitAt + 1, cend,
                                            forkDepth - 1, lo, splitAt->pos, cbeg, splitAt,
                                            splitAt->pos, tokens_[splitAt->pos].type);
                        }
                        int lhs = parseRange(lo, splitAt->pos, depth, cbeg, splitAt, 0);
                        int rhs = parseRange(splitAt->pos + 1, hi, depth,
                                             splitAt + 1, cend, 0);
                        return emitAt(splitAt->pos,
                                      {binaryKind(tokens_[splitAt->pos].type),
                                       lhs, rhs, 0, 0.0});
                    }
                    int acc = parseRange(lo, cbeg->pos, depth, cend, cend, 0);
                    for (auto it = cbeg; it != cend; ++it) {
                        const std::size_t nlo = it->pos + 1;
                        const std::size_t nhi = (it + 1 != cend) ? (it+1)->pos : hi;
                        int rhs = parseRange(nlo, nhi, depth, cend, cend, 0);
                        acc = emitAt(it->pos,
                                     {binaryKind(tokens_[it->pos].type), acc, rhs, 0, 0.0});
                    }
                    return acc;
                } else {
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
                        acc = emitAt(it->pos,
                                     {binaryKind(tokens_[it->pos].type),
                                      parts[i], acc, 0, 0.0});
                    }
                    return acc;
                }
            }
        }

        const CIt splitIt = findSplit(cbeg, cend, lo, hi);
        if (splitIt != cend) {
            if (splitIt->unary) {
                int operand = parseRange(splitIt->pos + 1, hi, depth,
                                         splitIt + 1, cend, forkDepth);
                const ArenaAst::K k = (tokens_[splitIt->pos].type == TokenType::Minus)
                                      ? ArenaAst::K::Neg : ArenaAst::K::Pos;
                return emitAt(splitIt->pos, {k, operand, -1, 0, 0.0});
            }
            if (forkDepth > 0 && hi - lo > forkThreshold_) {
                return forkJoin(splitIt->pos + 1, hi, depth, splitIt + 1, cend,
                                forkDepth - 1, lo, splitIt->pos, cbeg, splitIt,
                                splitIt->pos, tokens_[splitIt->pos].type);
            }
            int lhs = parseRange(lo,               splitIt->pos, depth, cbeg,        splitIt, forkDepth);
            int rhs = parseRange(splitIt->pos + 1, hi,           depth, splitIt + 1, cend,    forkDepth);
            return emitAt(splitIt->pos,
                          {binaryKind(tokens_[splitIt->pos].type), lhs, rhs, 0, 0.0});
        }

        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi - 1) {
            auto [b2, e2] = candRange(lo + 1, hi - 1, depth + 1);
            return parseRange(lo + 1, hi - 1, depth + 1, b2, e2, forkDepth);
        }
        if (hi - lo == 1) {
            const Token& t = tokens_[lo];
            if (t.type == TokenType::Number)
                return emitAt(lo, {ArenaAst::K::Num, -1, -1, 0, t.value});
            if (t.type == TokenType::Ident)
                return emitAt(lo, {ArenaAst::K::Var, -1, -1, static_cast<int>(t.value), 0.0});
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
        return 0.0;
    }

public:
    MultipassPool(int maxForkDepth, const char* name)
        : MpForkCore(maxForkDepth), name_(name) {}

    const char* name() const override { return name_; }

    double eval(std::string_view src, const double* vars = nullptr) override {
        auto s = setup(src);
        vars_ = vars;
        nodes_.resize(tokens_.size());
        const int root = parseRange(0, s.n, 0, s.cbeg, s.cend, maxForkDepth_);
        return evalNode(root);
    }
};

// ---- direct-eval variant: fork-join returns the value, no tree -------------
// parseRangeD computes a double instead of emitting a node, so evaluation is
// fused into the parallel recursion — no node array and no serial evalNode walk.
// Same split structure as MultipassPool, so results are bit-identical to par1.
class MultipassDirectFork final : public IEvaluator, MpForkCore {
    const char* name_;

    double forkJoinD(std::size_t rlo, std::size_t rhi, int depth, CIt rbeg, CIt rend,
                     int fd, std::size_t llo, std::size_t lhi, CIt lbeg, CIt lend,
                     TokenType opType) {
        const auto [lhs, rhs] = forkJoinImpl<double>(
            [&] { return parseRangeD(rlo, rhi, depth, rbeg, rend, fd); },
            [&] { return parseRangeD(llo, lhi, depth, lbeg, lend, fd); });
        return applyBin(opType, lhs, rhs);
    }

    double parseRangeD(std::size_t lo, std::size_t hi, int depth,
                       CIt cbeg, CIt cend, int forkDepth) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        if (cbeg != cend) {
            const int  chainPrec = cbeg->prec;
            const bool chainRa   = cbeg->rightAssoc;
            bool flat = true;
            for (auto it = cbeg; it != cend; ++it)
                if (it->unary || it->prec != chainPrec) { flat = false; break; }

            if (flat) {
                if (!chainRa) {
                    const CIt splitAt = findChainBisect(cbeg, cend, lo, hi);
                    if (splitAt != cend) {
                        const TokenType op = tokens_[splitAt->pos].type;
                        if (forkDepth > 0 && hi - lo > forkThreshold_) {
                            return forkJoinD(splitAt->pos + 1, hi, depth, splitAt + 1, cend,
                                             forkDepth - 1, lo, splitAt->pos, cbeg, splitAt, op);
                        }
                        double lhs = parseRangeD(lo, splitAt->pos, depth, cbeg, splitAt, 0);
                        double rhs = parseRangeD(splitAt->pos + 1, hi, depth,
                                                 splitAt + 1, cend, 0);
                        return applyBin(op, lhs, rhs);
                    }
                    double acc = parseRangeD(lo, cbeg->pos, depth, cend, cend, 0);
                    for (auto it = cbeg; it != cend; ++it) {
                        const std::size_t nlo = it->pos + 1;
                        const std::size_t nhi = (it + 1 != cend) ? (it+1)->pos : hi;
                        double rhs = parseRangeD(nlo, nhi, depth, cend, cend, 0);
                        acc = applyBin(tokens_[it->pos].type, acc, rhs);
                    }
                    return acc;
                } else {
                    std::vector<double> parts;
                    parts.reserve(static_cast<std::size_t>(cend - cbeg) + 1);
                    std::size_t prevLo = lo;
                    for (auto it = cbeg; it != cend; ++it) {
                        parts.push_back(parseRangeD(prevLo, it->pos, depth, cend, cend, 0));
                        prevLo = it->pos + 1;
                    }
                    parts.push_back(parseRangeD(prevLo, hi, depth, cend, cend, 0));
                    double acc = parts.back();
                    for (std::size_t i = parts.size() - 1; i-- > 0; )
                        acc = std::pow(parts[i], acc);   // only ^ reaches the right fold
                    return acc;
                }
            }
        }

        const CIt splitIt = findSplit(cbeg, cend, lo, hi);
        if (splitIt != cend) {
            if (splitIt->unary) {
                double operand = parseRangeD(splitIt->pos + 1, hi, depth,
                                             splitIt + 1, cend, forkDepth);
                return (tokens_[splitIt->pos].type == TokenType::Minus) ? -operand : +operand;
            }
            const TokenType op = tokens_[splitIt->pos].type;
            if (forkDepth > 0 && hi - lo > forkThreshold_) {
                return forkJoinD(splitIt->pos + 1, hi, depth, splitIt + 1, cend,
                                 forkDepth - 1, lo, splitIt->pos, cbeg, splitIt, op);
            }
            double lhs = parseRangeD(lo,               splitIt->pos, depth, cbeg,        splitIt, forkDepth);
            double rhs = parseRangeD(splitIt->pos + 1, hi,           depth, splitIt + 1, cend,    forkDepth);
            return applyBin(op, lhs, rhs);
        }

        if (tokens_[lo].type == TokenType::LParen && parenMatch_[lo] == hi - 1) {
            auto [b2, e2] = candRange(lo + 1, hi - 1, depth + 1);
            return parseRangeD(lo + 1, hi - 1, depth + 1, b2, e2, forkDepth);
        }
        if (hi - lo == 1) {
            const Token& t = tokens_[lo];
            if (t.type == TokenType::Number) return t.value;
            if (t.type == TokenType::Ident)  return vars_ ? vars_[static_cast<int>(t.value)] : 0.0;
        }
        throw std::runtime_error("syntax error at position " +
                                 std::to_string(tokens_[lo].pos));
    }

public:
    MultipassDirectFork(int maxForkDepth, const char* name)
        : MpForkCore(maxForkDepth), name_(name) {}

    const char* name() const override { return name_; }

    double eval(std::string_view src, const double* vars = nullptr) override {
        auto s = setup(src);
        vars_ = vars;
        return parseRangeD(0, s.n, 0, s.cbeg, s.cend, maxForkDepth_);
    }
};

// ---- serial-floor probe: run the prologue (tokenize + buildCandidates) only -
// Times everything the parallel variants must do serially before any fork —
// the hard floor no amount of parse parallelism can go below.
class MpSetupOnly final : public IEvaluator, MpForkCore {
public:
    MpSetupOnly() : MpForkCore(0) {}   // 0 workers: no pool threads
    const char* name() const override { return "mp-setup-only"; }
    double eval(std::string_view src, const double* = nullptr) override {
        auto s = setup(src);
        // touch both results so neither buildCandidates nor candRange is elided
        return static_cast<double>(s.n) + static_cast<double>(s.cend - s.cbeg);
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_mp_setup_only() {
    return std::make_unique<MpSetupOnly>();
}

std::unique_ptr<IEvaluator> make_multipass_pool2() {
    return std::make_unique<MultipassPool>(1, "multipass-pool2");
}
std::unique_ptr<IEvaluator> make_multipass_pool4() {
    return std::make_unique<MultipassPool>(2, "multipass-pool4");
}
std::unique_ptr<IEvaluator> make_multipass_pool8() {
    return std::make_unique<MultipassPool>(3, "multipass-pool8");
}

std::unique_ptr<IEvaluator> make_multipass_dfork2() {
    return std::make_unique<MultipassDirectFork>(1, "multipass-dfork2");
}
std::unique_ptr<IEvaluator> make_multipass_dfork4() {
    return std::make_unique<MultipassDirectFork>(2, "multipass-dfork4");
}
std::unique_ptr<IEvaluator> make_multipass_dfork8() {
    return std::make_unique<MultipassDirectFork>(3, "multipass-dfork8");
}

}  // namespace mp
