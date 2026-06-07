#include "parser/lexer.hpp"
#include "parser/parser.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mp {
namespace {

// In one sentence: recursively find the lowest-precedence operator at paren-depth
// zero, make it the tree's root, and recurse on the two halves to either side.
//
// "Divide-and-conquer" / recursive-split parsing. Unlike classic recursive
// descent (one token-by-token pass with precedence baked into the call chain),
// this re-scans each sub-range to locate its splitting operator, so it is the
// naive O(n^2)-worst-case sibling -- but its sub-ranges are independent, which
// makes it the most naturally *parallelizable* of the strategies.
//
// Precedence (lower binds looser -> sits higher -> is the root):
//   binary + -  : 1   (left-assoc  -> split on the RIGHTMOST one)
//   binary * /  : 2   (left-assoc  -> split on the RIGHTMOST one)
//   unary  + -  : 3   (prefix; only a root candidate when it leads the range)
//   ^           : 4   (right-assoc -> split on the LEFTMOST one)
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

class MultiPass final : public IParser {
public:
    const char* name() const override { return "multipass"; }

    ExprPtr parse(std::string_view src) override {
        tokens_ = tokenize(src);
        return parseRange(0, tokens_.size() - 1);  // exclude trailing End
    }

private:
    std::vector<Token> tokens_;

    ExprPtr parseRange(std::size_t lo, std::size_t hi) {
        if (lo >= hi) throw std::runtime_error("empty sub-expression");

        // Locate the root: the lowest-precedence operator at depth 0.
        int  depth = 0;
        bool expectOperand = true;
        int  bestPrec = 1000;
        std::size_t bestIdx = 0;
        bool bestUnary = false;
        bool found = false;

        for (std::size_t i = lo; i < hi; ++i) {
            switch (tokens_[i].type) {
                case TokenType::LParen: ++depth; expectOperand = true;  break;
                case TokenType::RParen: --depth; expectOperand = false; break;
                case TokenType::Number:
                case TokenType::Ident:  expectOperand = false; break;
                case TokenType::Plus:
                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Caret:
                    if (depth == 0) {
                        if (expectOperand) {
                            // prefix unary; only the leading one can be the root
                            if (i == lo && (tokens_[i].type == TokenType::Plus ||
                                            tokens_[i].type == TokenType::Minus) &&
                                kUnaryPrec < bestPrec) {
                                bestPrec = kUnaryPrec; bestIdx = i; bestUnary = true; found = true;
                            }
                        } else {
                            const int  p  = binPrec(tokens_[i].type);
                            const bool ra = (tokens_[i].type == TokenType::Caret);
                            if (p < bestPrec) {
                                bestPrec = p; bestIdx = i; bestUnary = false; found = true;
                            } else if (p == bestPrec && !bestUnary && !ra) {
                                bestIdx = i;  // left-assoc: keep the rightmost
                            }
                            // right-assoc (^): keep the leftmost -> do not replace
                        }
                    }
                    expectOperand = true;
                    break;
                case TokenType::End: break;
            }
        }

        if (found) {
            if (bestUnary) {
                return unary(tokens_[bestIdx].type, parseRange(bestIdx + 1, hi));
            }
            return binary(tokens_[bestIdx].type,
                          parseRange(lo, bestIdx), parseRange(bestIdx + 1, hi));
        }

        // No depth-0 operator: a parenthesized group, or a single primary.
        if (tokens_[lo].type == TokenType::LParen) {
            // Strip the outer pair only if the '(' at lo matches the ')' at hi-1.
            int d = 0;
            std::size_t k = lo;
            for (; k < hi; ++k) {
                if (tokens_[k].type == TokenType::LParen) ++d;
                else if (tokens_[k].type == TokenType::RParen && --d == 0) break;
            }
            if (k == hi - 1) return parseRange(lo + 1, hi - 1);
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
