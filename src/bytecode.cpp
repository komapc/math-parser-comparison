#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mp {
namespace {

// In one sentence: shunting-yard compiles to a compact opcode stream + constant
// pool, then a switch-dispatch stack VM executes it.
//
// Bytecode + stack VM. Phase 1: shunting-yard compiles to a compact opcode byte
// stream plus a separate constant pool (denser than the tagged-token RPN form).
// Phase 2: a switch-dispatch VM executes the byte stream over a value stack.
enum class Bc : std::uint8_t { Push, Add, Sub, Mul, Div, Pow, Neg };

Bc binOpcode(TokenType t) {
    switch (t) {
        case TokenType::Plus:  return Bc::Add;
        case TokenType::Minus: return Bc::Sub;
        case TokenType::Star:  return Bc::Mul;
        case TokenType::Slash: return Bc::Div;
        case TokenType::Caret: return Bc::Pow;
        default:               throw std::runtime_error("invalid operator");
    }
}

struct Program {
    std::vector<std::uint8_t> code;
    std::vector<double>       consts;
};

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

class Bytecode final : public IEvaluator {
public:
    const char* name() const override { return "bytecode-vm"; }

    double eval(std::string_view src) override {
        const Program prog = compile(src);

        std::vector<double> st;
        st.reserve(prog.consts.size() + 1);
        std::size_t ci = 0;
        for (const std::uint8_t opc : prog.code) {
            switch (static_cast<Bc>(opc)) {
                case Bc::Push: st.push_back(prog.consts[ci++]); break;
                case Bc::Neg:  st.back() = -st.back(); break;
                default: {
                    const double r = st.back(); st.pop_back();
                    double& l = st.back();
                    switch (static_cast<Bc>(opc)) {
                        case Bc::Add: l = l + r; break;
                        case Bc::Sub: l = l - r; break;
                        case Bc::Mul: l = l * r; break;
                        case Bc::Div: l = l / r; break;
                        case Bc::Pow: l = std::pow(l, r); break;
                        default: break;
                    }
                }
            }
        }
        return st.back();
    }

private:
    static Program compile(std::string_view src) {
        const std::vector<Token> tokens = tokenize(src);
        Program p;
        std::vector<Op> ops;

        auto emit = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                if (op.type == TokenType::Minus)
                    p.code.push_back(static_cast<std::uint8_t>(Bc::Neg));
                // unary plus: identity
            } else {
                p.code.push_back(static_cast<std::uint8_t>(binOpcode(op.type)));
            }
        };

        bool expectOperand = true;
        for (const Token& tok : tokens) {
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    p.code.push_back(static_cast<std::uint8_t>(Bc::Push));
                    p.consts.push_back(tok.value);
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
                        const int prec = binPrec(tok.type);
                        const bool ra = (tok.type == TokenType::Caret);
                        while (!ops.empty() && !ops.back().lparen &&
                               (ops.back().prec > prec || (ops.back().prec == prec && !ra))) {
                            Op o = ops.back(); ops.pop_back(); emit(o);
                        }
                        ops.push_back(Op{tok.type, prec, ra, false, false});
                        expectOperand = true;
                    }
                    break;
                case TokenType::End:
                    if (expectOperand) throw std::runtime_error("unexpected end of input");
                    break;
            }
        }
        while (!ops.empty()) { Op o = ops.back(); ops.pop_back(); emit(o); }
        if (p.code.empty()) throw std::runtime_error("invalid expression");
        return p;
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_bytecode() {
    return std::make_unique<Bytecode>();
}

}  // namespace mp
