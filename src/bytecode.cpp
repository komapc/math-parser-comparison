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
enum class Bc : std::uint8_t { Push, LoadVar, Add, Sub, Mul, Div, Pow, Neg };

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

    double eval(std::string_view src, const double* vars = nullptr) override {
        tokens_ = tokenize(src);
        code_.clear();
        consts_.clear();
        ops_.clear();
        compile();

        st_.clear();
        std::size_t ci = 0;
        for (const std::uint8_t opc : code_) {
            switch (static_cast<Bc>(opc)) {
                case Bc::Push:    st_.push_back(consts_[ci++]); break;
                case Bc::LoadVar: st_.push_back(vars ? vars[static_cast<std::size_t>(consts_[ci++])] : 0.0); break;
                case Bc::Neg:  st_.back() = -st_.back(); break;
                default: {
                    const double r = st_.back(); st_.pop_back();
                    double& l = st_.back();
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
        return st_.back();
    }

private:
    std::vector<Token>        tokens_;
    std::vector<std::uint8_t> code_;
    std::vector<double>       consts_;
    std::vector<Op>           ops_;
    std::vector<double>       st_;

    void compile() {
        auto emit = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                if (op.type == TokenType::Minus)
                    code_.push_back(static_cast<std::uint8_t>(Bc::Neg));
            } else {
                code_.push_back(static_cast<std::uint8_t>(binOpcode(op.type)));
            }
        };

        bool expectOperand = true;
        for (const Token& tok : tokens_) {
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    code_.push_back(static_cast<std::uint8_t>(Bc::Push));
                    consts_.push_back(tok.value);
                    expectOperand = false;
                    break;
                case TokenType::Ident:
                    if (!expectOperand) throw std::runtime_error("unexpected variable");
                    code_.push_back(static_cast<std::uint8_t>(Bc::LoadVar));
                    consts_.push_back(tok.value);
                    expectOperand = false;
                    break;
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
                        const int prec = binPrec(tok.type);
                        const bool ra = (tok.type == TokenType::Caret);
                        while (!ops_.empty() && !ops_.back().lparen &&
                               (ops_.back().prec > prec || (ops_.back().prec == prec && !ra))) {
                            Op o = ops_.back(); ops_.pop_back(); emit(o);
                        }
                        ops_.push_back(Op{tok.type, prec, ra, false, false});
                        expectOperand = true;
                    }
                    break;
                case TokenType::End:
                    if (expectOperand) throw std::runtime_error("unexpected end of input");
                    break;
            }
        }
        while (!ops_.empty()) { Op o = ops_.back(); ops_.pop_back(); emit(o); }
        if (code_.empty()) throw std::runtime_error("invalid expression");
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_bytecode() {
    return std::make_unique<Bytecode>();
}

}  // namespace mp
