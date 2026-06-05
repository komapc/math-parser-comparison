#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace mp {
namespace {

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
        const std::vector<RTok> code = compile(src);

        std::vector<double> st;
        st.reserve(code.size());
        for (const RTok& t : code) {
            switch (t.kind) {
                case RKind::Num: st.push_back(t.value); break;
                case RKind::Neg: st.back() = -st.back(); break;
                default: {
                    const double r = st.back(); st.pop_back();
                    double& l = st.back();
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
        return st.back();
    }

private:
    static std::vector<RTok> compile(std::string_view src) {
        const std::vector<Token> tokens = tokenize(src);
        std::vector<RTok> out;
        std::vector<Op> ops;

        auto emit = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                // unary plus is identity -> emit nothing; only minus negates
                if (op.type == TokenType::Minus) out.push_back(RTok{RKind::Neg, 0.0});
            } else {
                out.push_back(RTok{binKind(op.type), 0.0});
            }
        };

        bool expectOperand = true;
        for (const Token& tok : tokens) {
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    out.push_back(RTok{RKind::Num, tok.value});
                    expectOperand = false;
                    break;
                case TokenType::Ident:
                    throw std::runtime_error("variables not supported by this evaluator");
                case TokenType::LParen:
                    ops.push_back(Op{tok.type, 0, false, false, true});
                    expectOperand = true;
                    break;
                case TokenType::RParen:
                    while (!ops.empty() && !ops.back().lparen) { Op o = ops.back(); ops.pop_back(); emit(o); }
                    if (ops.empty()) throw std::runtime_error("mismatched parenthesis");
                    ops.pop_back();
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
                        ops.push_back(Op{tok.type, 3, true, true, false});
                    } else {
                        const int p = binPrec(tok.type);
                        const bool ra = (tok.type == TokenType::Caret);
                        while (!ops.empty() && !ops.back().lparen &&
                               (ops.back().prec > p || (ops.back().prec == p && !ra))) {
                            Op o = ops.back(); ops.pop_back(); emit(o);
                        }
                        ops.push_back(Op{tok.type, p, ra, false, false});
                        expectOperand = true;
                    }
                    break;
                case TokenType::End:
                    if (expectOperand) throw std::runtime_error("unexpected end of input");
                    break;
            }
        }
        while (!ops.empty()) { Op o = ops.back(); ops.pop_back(); emit(o); }
        if (out.empty()) throw std::runtime_error("invalid expression");
        return out;
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_rpn() {
    return std::make_unique<Rpn>();
}

}  // namespace mp
