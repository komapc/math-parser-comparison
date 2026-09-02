#include "parser/evaluator.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mp {
namespace {

// In one sentence: direct recursive descent that reads characters, not
// tokens -- the lexer is fused into the grammar, so no token array is ever
// materialised.
//
// Same grammar and the same lexical rules as the shared tokenizer (whitespace
// set, number syntax incl. the 1e400 -> inf convention, single-letter a-z/A-Z
// variables, error positions), but every other strategy pays for
// tokenize()'s token vector -- about half of a direct evaluator's time in C++
// (see floor_bench: tokenize alone vs direct-recursive-descent). This one is
// the control for that cost: it is what direct-recursive-descent would be if
// lexing were free.
//
// Whitespace is consumed exactly once, right after each token, by take():
// the grammar then never has to think about it. Each rule compares the next
// character against the one or two it can accept, so lexing here is
// context-sensitive -- after an operand only operator characters are tested,
// where a general tokenizer would classify the character first and let the
// parser reject the class afterwards. That, not the missing allocation, is
// where the remaining gap to a streaming tokenizer comes from.
class DirectScannerless final : public IEvaluator {
public:
    const char* name() const override { return "direct-scannerless"; }

    double eval(std::string_view src, const double* vars = nullptr) override {
        base_ = p_ = src.data();
        end_ = p_ + src.size();
        vars_ = vars;
        skip();
        const double v = expr();
        if (p_ != end_) throw std::runtime_error("unexpected token at position " + std::to_string(pos()));
        return v;
    }

private:
    const char*   p_    = nullptr;
    const char*   end_  = nullptr;
    const char*   base_ = nullptr;
    const double* vars_ = nullptr;

    std::size_t pos() const { return static_cast<std::size_t>(p_ - base_); }
    static bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool isLetter(char c) { const char l = static_cast<char>(c | 0x20); return l >= 'a' && l <= 'z'; }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }

    void skip() { while (p_ < end_ && isWs(*p_)) ++p_; }
    // Consume `c` if it is next, then the whitespace after it.
    bool take(char c) {
        if (p_ == end_ || *p_ != c) return false;
        ++p_; skip(); return true;
    }

    double expr() {
        double l = term();
        for (;;) {
            if (take('+'))      l += term();
            else if (take('-')) l -= term();
            else return l;
        }
    }
    double term() {
        double l = unaryRule();
        for (;;) {
            if (take('*'))      l *= unaryRule();
            else if (take('/')) l /= unaryRule();
            else return l;
        }
    }
    double unaryRule() {
        if (take('-')) return -unaryRule();
        if (take('+')) return +unaryRule();
        return power();
    }
    double power() {
        const double base = primary();
        return take('^') ? std::pow(base, unaryRule()) : base;
    }
    double primary() {
        if (take('(')) {
            const double v = expr();
            if (!take(')')) throw std::runtime_error("expected ')' at position " + std::to_string(pos()));
            return v;
        }
        if (p_ < end_ && (isLetter(*p_) || *p_ == '_')) {
            // Maximal identifier run; only single letters are variables
            // (mirrors tokenize()).
            const char* q = p_;
            while (q < end_ && (isLetter(*q) || isDigit(*q) || *q == '_')) ++q;
            if (q - p_ != 1 || *p_ == '_') throw std::runtime_error("unknown identifier at position " + std::to_string(pos()));
            const int idx = (*p_ | 0x20) - 'a';
            p_ = q; skip();
            return vars_ ? vars_[idx] : 0.0;
        }
        if (p_ < end_ && (isDigit(*p_) || *p_ == '.')) {
            double v = 0.0;
            auto [q, ec] = std::from_chars(p_, end_, v);
            if (ec == std::errc::result_out_of_range) {
                // Overflow/underflow is a value (inf / 0), not a syntax error
                // -- same convention as tokenize().
                v = std::strtod(std::string(p_, q).c_str(), nullptr);
                ec = std::errc();
            }
            if (ec != std::errc() || q == p_) throw std::runtime_error("invalid number at position " + std::to_string(pos()));
            p_ = q; skip();
            return v;
        }
        throw std::runtime_error("expected number or '(' at position " + std::to_string(pos()));
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_direct_scannerless() {
    return std::make_unique<DirectScannerless>();
}

}  // namespace mp
