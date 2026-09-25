#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mp {
namespace {

// In one sentence: shunting-yard whose operand stack holds running numeric values,
// computing the result during the single scan with no AST.
//
// Dijkstra's original shunting-yard, evaluating to a number directly: the output
// "stack" holds doubles rather than AST nodes. Same precedence/associativity and
// unary-minus handling as src/shunting_yard.cpp.
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

double applyBinary(TokenType op, double l, double r) {
    switch (op) {
        case TokenType::Plus:  return l + r;
        case TokenType::Minus: return l - r;
        case TokenType::Star:  return l * r;
        case TokenType::Slash: return l / r;
        case TokenType::Caret: return std::pow(l, r);
        default:               throw std::runtime_error("invalid operator");
    }
}

class DirectShuntingYard final : public IEvaluator {
public:
    const char* name() const override { return "direct-shunting-yard"; }

    double eval(std::string_view src, const double* vars = nullptr) override {
        // Streaming lexer: shunting-yard reads its input once, left to right.
        Lexer lx(src);
        // Each token pushes at most one value / one op, and there is at most
        // one token per source byte, so the source length bounds both stacks.
        // Pre-sized raw buffers indexed by locals — the same treatment as
        // ReverseFold (multipass_reverse_fold.cpp).
        const std::size_t bound = src.size() + 1;
        if (vals_.size() < bound) {
            vals_.resize(bound);
            ops_.resize(bound);
        }
        double* const vals = vals_.data();
        Op*     const ops  = ops_.data();
        std::uint32_t vTop = 0, oTop = 0;

        auto fold = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                if (vTop == 0) throw std::runtime_error("missing operand");
                const double a = vals[vTop - 1];
                vals[vTop - 1] = (op.type == TokenType::Minus) ? -a : a;
            } else {
                if (vTop < 2) throw std::runtime_error("missing operand");
                const double r = vals[--vTop];
                const double l = vals[--vTop];
                vals[vTop++] = applyBinary(op.type, l, r);
            }
        };

        bool expectOperand = true;
        for (;;) {
            const Token tok = lx.next();
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    vals[vTop++] = tok.value;
                    expectOperand = false;
                    break;
                case TokenType::Ident:
                    if (!expectOperand) throw std::runtime_error("unexpected variable");
                    vals[vTop++] = vars ? vars[static_cast<int>(tok.value)] : 0.0;
                    expectOperand = false;
                    break;
                case TokenType::LParen:
                    if (!expectOperand) throw std::runtime_error("unexpected '('");
                    ops[oTop++] = Op{tok.type, 0, false, false, true};
                    expectOperand = true;
                    break;
                case TokenType::RParen:
                    if (expectOperand) throw std::runtime_error("empty parentheses");
                    while (oTop != 0 && !ops[oTop - 1].lparen) fold(ops[--oTop]);
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
                        const int p = binPrec(tok.type);
                        const bool ra = (tok.type == TokenType::Caret);
                        while (oTop != 0 && !ops[oTop - 1].lparen &&
                               (ops[oTop - 1].prec > p || (ops[oTop - 1].prec == p && !ra))) {
                            fold(ops[--oTop]);
                        }
                        ops[oTop++] = Op{tok.type, p, ra, false, false};
                        expectOperand = true;
                    }
                    break;
                case TokenType::End:
                    if (expectOperand) throw std::runtime_error("unexpected end of input");
                    break;
            }
            if (tok.type == TokenType::End) break;
        }
        while (oTop != 0) fold(ops[--oTop]);
        if (vTop != 1) throw std::runtime_error("invalid expression");
        return vals[0];
    }

private:
    std::vector<double> vals_;
    std::vector<Op>     ops_;
};

}  // namespace

std::unique_ptr<IEvaluator> make_direct_shunting_yard() {
    return std::make_unique<DirectShuntingYard>();
}

}  // namespace mp
