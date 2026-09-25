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
        // Each token emits at most one opcode / constant / op-stack entry and
        // the VM pushes at most one value per constant; there is at most one
        // token per source byte, so the source length bounds all four buffers.
        // Pre-sized raw buffers indexed by locals — the same treatment as
        // ReverseFold (multipass_reverse_fold.cpp).
        const std::size_t bound = src.size() + 1;
        if (st_.size() < bound) {
            code_.resize(bound);
            consts_.resize(bound);
            ops_.resize(bound);
            st_.resize(bound);
        }
        const std::uint32_t codeLen = compile(src);

        const std::uint8_t* const code   = code_.data();
        const double*       const consts = consts_.data();
        double*             const st     = st_.data();
        std::uint32_t sTop = 0, ci = 0;
        for (std::uint32_t pc = 0; pc < codeLen; ++pc) {
            const std::uint8_t opc = code[pc];
            switch (static_cast<Bc>(opc)) {
                case Bc::Push:    st[sTop++] = consts[ci++]; break;
                case Bc::LoadVar: {
                    const auto idx = static_cast<std::size_t>(consts[ci++]);
                    st[sTop++] = vars ? vars[idx] : 0.0;
                    break;
                }
                case Bc::Neg:  st[sTop - 1] = -st[sTop - 1]; break;
                default: {
                    const double r = st[--sTop];
                    double& l = st[sTop - 1];
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
        return st[sTop - 1];
    }

private:
    std::vector<std::uint8_t> code_;
    std::vector<double>       consts_;
    std::vector<Op>           ops_;
    std::vector<double>       st_;

    // Streaming lexer: the shunting-yard compile reads its input once.
    // Returns the opcode count; buffers are pre-sized by eval().
    std::uint32_t compile(std::string_view src) {
        std::uint8_t* const code   = code_.data();
        double*       const consts = consts_.data();
        Op*           const ops    = ops_.data();
        std::uint32_t cTop = 0, kTop = 0, oTop = 0;

        auto emit = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                if (op.type == TokenType::Minus)
                    code[cTop++] = static_cast<std::uint8_t>(Bc::Neg);
            } else {
                code[cTop++] = static_cast<std::uint8_t>(binOpcode(op.type));
            }
        };

        bool expectOperand = true;
        Lexer lx(src);
        for (;;) {
            const Token tok = lx.next();
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    code[cTop++] = static_cast<std::uint8_t>(Bc::Push);
                    consts[kTop++] = tok.value;
                    expectOperand = false;
                    break;
                case TokenType::Ident:
                    if (!expectOperand) throw std::runtime_error("unexpected variable");
                    code[cTop++] = static_cast<std::uint8_t>(Bc::LoadVar);
                    consts[kTop++] = tok.value;
                    expectOperand = false;
                    break;
                case TokenType::LParen:
                    if (!expectOperand) throw std::runtime_error("unexpected '('");
                    ops[oTop++] = Op{tok.type, 0, false, false, true};
                    expectOperand = true;
                    break;
                case TokenType::RParen:
                    if (expectOperand) throw std::runtime_error("empty parentheses");
                    while (oTop != 0 && !ops[oTop - 1].lparen) emit(ops[--oTop]);
                    if (oTop == 0) throw std::runtime_error("mismatched parenthesis");
                    --oTop;
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
                        ops[oTop++] = Op{tok.type, 3, true, true, false};
                    } else {
                        const int prec = binPrec(tok.type);
                        const bool ra = (tok.type == TokenType::Caret);
                        while (oTop != 0 && !ops[oTop - 1].lparen &&
                               (ops[oTop - 1].prec > prec || (ops[oTop - 1].prec == prec && !ra))) {
                            emit(ops[--oTop]);
                        }
                        ops[oTop++] = Op{tok.type, prec, ra, false, false};
                        expectOperand = true;
                    }
                    break;
                case TokenType::End:
                    if (expectOperand) throw std::runtime_error("unexpected end of input");
                    break;
            }
            if (tok.type == TokenType::End) break;
        }
        while (oTop != 0) emit(ops[--oTop]);
        if (cTop == 0) throw std::runtime_error("invalid expression");
        return cTop;
    }
};

}  // namespace

std::unique_ptr<IEvaluator> make_bytecode() {
    return std::make_unique<Bytecode>();
}

}  // namespace mp
