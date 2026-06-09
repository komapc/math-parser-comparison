#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"

#include <cmath>
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
        tokens_ = tokenize(src);
        vals_.clear();
        ops_.clear();

        auto fold = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                if (vals_.empty()) throw std::runtime_error("missing operand");
                const double a = vals_.back();
                vals_.back() = (op.type == TokenType::Minus) ? -a : a;
            } else {
                if (vals_.size() < 2) throw std::runtime_error("missing operand");
                const double r = vals_.back(); vals_.pop_back();
                const double l = vals_.back(); vals_.pop_back();
                vals_.push_back(applyBinary(op.type, l, r));
            }
        };

        bool expectOperand = true;
        for (const Token& tok : tokens_) {
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    vals_.push_back(tok.value);
                    expectOperand = false;
                    break;
                case TokenType::Ident:
                    if (!expectOperand) throw std::runtime_error("unexpected variable");
                    vals_.push_back(vars ? vars[static_cast<int>(tok.value)] : 0.0);
                    expectOperand = false;
                    break;
                case TokenType::LParen:
                    ops_.push_back(Op{tok.type, 0, false, false, true});
                    expectOperand = true;
                    break;
                case TokenType::RParen:
                    while (!ops_.empty() && !ops_.back().lparen) { Op o = ops_.back(); ops_.pop_back(); fold(o); }
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
                            Op o = ops_.back(); ops_.pop_back(); fold(o);
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
        while (!ops_.empty()) { Op o = ops_.back(); ops_.pop_back(); fold(o); }
        if (vals_.size() != 1) throw std::runtime_error("invalid expression");
        return vals_.back();
    }

private:
    std::vector<Token>  tokens_;
    std::vector<double> vals_;
    std::vector<Op>     ops_;
};

}  // namespace

std::unique_ptr<IEvaluator> make_direct_shunting_yard() {
    return std::make_unique<DirectShuntingYard>();
}

}  // namespace mp
