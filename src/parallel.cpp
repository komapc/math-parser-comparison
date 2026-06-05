#include "parser/parallel.hpp"

#include "parser/parser.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <thread>
#include <vector>

namespace mp {
namespace {

struct Range { std::size_t start; std::size_t len; };

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Advance past a numeric literal (digits, '.', and an exponent whose sign must
// NOT be mistaken for an operator, e.g. the '+' in "2e+5").
std::size_t skipNumber(std::string_view s, std::size_t i) {
    const std::size_t n = s.size();
    while (i < n && (isDigit(s[i]) || s[i] == '.')) ++i;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        while (i < n && isDigit(s[i])) ++i;
    }
    return i;
}

}  // namespace

ExprPtr parse_parallel(std::string_view src, unsigned threads) {
    // 1. Cheap character scan (no lexing, no allocation) to find depth-0 binary
    //    +/- separators and carve src into independent additive terms. This is
    //    the only sequential O(n) pass; the heavy lexing+AST work is parallel.
    std::vector<Range> terms;
    std::vector<TokenType> ops;  // joins terms; size == terms.size() - 1
    int depth = 0;
    bool expectOperand = true;
    std::size_t start = 0;
    const std::size_t n = src.size();

    for (std::size_t i = 0; i < n;) {
        const char c = src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
        if (isDigit(c) || c == '.') { i = skipNumber(src, i); expectOperand = false; continue; }
        switch (c) {
            case '(': ++depth; expectOperand = true;  break;
            case ')': --depth; expectOperand = false; break;
            case '*':
            case '/':
            case '^': expectOperand = true; break;
            case '+':
            case '-':
                if (depth == 0 && !expectOperand) {
                    terms.push_back({start, i - start});  // term ends before operator
                    ops.push_back(c == '+' ? TokenType::Plus : TokenType::Minus);
                    start = i + 1;                         // next term starts after it
                }
                expectOperand = true;
                break;
            default:
                // Unknown character: let the per-term lexer report it.
                break;
        }
        ++i;
    }
    terms.push_back({start, n - start});

    // 2. Parse the terms in parallel; each worker lexes+parses its own slices,
    //    writing into disjoint result slots (no synchronization needed).
    const std::size_t m = terms.size();
    std::vector<ExprPtr> results(m);
    std::vector<std::exception_ptr> errors(m);

    auto worker = [&](std::size_t lo, std::size_t hi) {
        auto parser = make_recursive_descent();  // one instance per thread
        for (std::size_t k = lo; k < hi; ++k) {
            try {
                results[k] = parser->parse(src.substr(terms[k].start, terms[k].len));
            } catch (...) {
                errors[k] = std::current_exception();
            }
        }
    };

    unsigned t = std::max(1u, threads);
    t = static_cast<unsigned>(std::min<std::size_t>(t, m));
    if (t <= 1) {
        worker(0, m);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(t);
        const std::size_t chunk = (m + t - 1) / t;
        for (unsigned w = 0; w < t; ++w) {
            const std::size_t lo = std::min(m, static_cast<std::size_t>(w) * chunk);
            const std::size_t hi = std::min(m, lo + chunk);
            if (lo < hi) pool.emplace_back(worker, lo, hi);
        }
        for (auto& th : pool) th.join();
    }

    for (auto& ep : errors) {
        if (ep) std::rethrow_exception(ep);
    }

    // 3. Fold left-associatively (cheap, sequential) — reproduces the sequential
    //    result because '+'/'-' are left-associative.
    ExprPtr acc = std::move(results[0]);
    for (std::size_t k = 1; k < m; ++k) {
        acc = binary(ops[k - 1], std::move(acc), std::move(results[k]));
    }
    return acc;
}

}  // namespace mp
