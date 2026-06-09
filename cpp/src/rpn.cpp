#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace mp {
namespace {

// In one sentence: shunting-yard emits a flat postfix (RPN) token sequence, then
// a value stack walks that sequence to produce the result.
//
// RPN / postfix + stack machine. Phase 1: shunting-yard emits a flat postfix
// sequence of tagged tokens (instead of folding into an AST or a value).
// Phase 2: a value stack walks that sequence. One allocation for the output
// vector; the run is a tight, cache-friendly linear pass.
enum class RKind { Num, Add, Sub, Mul, Div, Pow, Neg };

struct RTok {
    RKind  kind;
    double value;  // used when kind == Num
};

RKind binKind(TokenType t) {
    switch (t) {
        case TokenType::Plus:  return RKind::Add;
        case TokenType::Minus: return RKind::Sub;
        case TokenType::Star:  return RKind::Mul;
        case TokenType::Slash: return RKind::Div;
        case TokenType::Caret: return RKind::Pow;
        default:               throw std::runtime_error("invalid operator");
    }
}

struct Op {
    TokenType type;
    int  prec;
    bool rightAssoc;
    bool unary;
    bool lparen;
};

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

class Rpn final : public IEvaluator {
public:
    const char* name() const override { return "rpn-stack"; }

    double eval(std::string_view src) override {
        tokens_ = tokenize(src);
        out_.clear();
        ops_.clear();
        compile();

        st_.clear();
        for (const RTok& t : out_) {
            switch (t.kind) {
                case RKind::Num: st_.push_back(t.value); break;
                case RKind::Neg: st_.back() = -st_.back(); break;
                default: {
                    const double r = st_.back(); st_.pop_back();
                    double& l = st_.back();
                    switch (t.kind) {
                        case RKind::Add: l = l + r; break;
                        case RKind::Sub: l = l - r; break;
                        case RKind::Mul: l = l * r; break;
                        case RKind::Div: l = l / r; break;
                        case RKind::Pow: l = std::pow(l, r); break;
                        default: break;
                    }
                }
            }
        }
        return st_.back();
    }

private:
    std::vector<Token> tokens_;
    std::vector<RTok>  out_;
    std::vector<Op>    ops_;
    std::vector<double> st_;

    void compile() {
        auto emit = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                if (op.type == TokenType::Minus) out_.push_back(RTok{RKind::Neg, 0.0});
            } else {
                out_.push_back(RTok{binKind(op.type), 0.0});
            }
        };

        bool expectOperand = true;
        for (const Token& tok : tokens_) {
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    out_.push_back(RTok{RKind::Num, tok.value});
                    expectOperand = false;
                    break;
                case TokenType::Ident:
                    throw std::runtime_error("variables not supported by this evaluator");
                case TokenType::LParen:
                    ops_.push_back(Op{tok.type, 0, false, false, true});
                    expectOperand = true;
                    break;
                case TokenType::RParen:
                    while (!ops_.empty() && !ops_.back().lparen) { Op o = ops_.back(); ops_.pop_back(); emit(o); }
                    if (ops_.empty()) throw std::runtime_error("mismatched parenthesis");
                    ops_.pop_back();
                    expectOperand = false;
                    break;
                case TokenType::Plus:
                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Caret:
                    if (expectOperand) {
                        if (tok.type != TokenType::Plus && tok.type != TokenType::Minus)
                            throw std::runtime_error("unexpected operator");
                        ops_.push_back(Op{tok.type, 3, true, true, false});
                    } else {
                        const int p = binPrec(tok.type);
                        const bool ra = (tok.type == TokenType::Caret);
                        while (!ops_.empty() && !ops_.back().lparen &&
                               (ops_.back().prec > p || (ops_.back().prec == p && !ra))) {
                            Op o = ops_.back(); ops_.pop_back(); emit(o);
                        }
                        ops_.push_back(Op{tok.type, p, ra, false, false});
                        expectOperand = true;
                    }
                    break;
                case TokenType::End:
                    if (expectOperand) throw std::runtime_error("unexpected end of input");
                    break;
            }
        }
        while (!ops_.empty()) { Op o = ops_.back(); ops_.pop_back(); emit(o); }
        if (out_.empty()) throw std::runtime_error("invalid expression");
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_rpn() {
    return std::make_unique<Rpn>();
}

}  // namespace mp
