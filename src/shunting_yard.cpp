#include "parser/lexer.hpp"
#include "parser/parser.hpp"

#include <stdexcept>
#include <vector>

namespace mp {
namespace {

// In one sentence: scan left-to-right, pushing operands and popping
// higher-or-equal-precedence operators off a stack to fold them into AST nodes.
//
// Strategy 2: Dijkstra's shunting-yard, building the shared AST.
// Two stacks (operands + operators); precedence lives in a table; unary minus
// is detected by context ("expecting operand"). Precedence:
//   + -        : 1
//   * /        : 2
//   unary + -  : 3   (prefix, right-associative)
//   ^          : 4   (right-associative)
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

class ShuntingYard final : public IParser {
public:
    const char* name() const override { return "shunting-yard"; }

    ExprPtr parse(std::string_view src) override {
        tokens_ = tokenize(src);
        operands_.clear();
        ops_.clear();

        auto fold = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                if (operands_.empty()) throw std::runtime_error("missing operand");
                ExprPtr e = std::move(operands_.back());
                operands_.pop_back();
                operands_.push_back(unary(op.type, std::move(e)));
            } else {
                if (operands_.size() < 2) throw std::runtime_error("missing operand");
                ExprPtr r = std::move(operands_.back());
                operands_.pop_back();
                ExprPtr l = std::move(operands_.back());
                operands_.pop_back();
                operands_.push_back(binary(op.type, std::move(l), std::move(r)));
            }
        };

        bool expectOperand = true;
        for (const Token& tok : tokens_) {
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    operands_.push_back(number(tok.value));
                    expectOperand = false;
                    break;

                case TokenType::Ident:
                    if (!expectOperand) throw std::runtime_error("unexpected variable");
                    operands_.push_back(variable(static_cast<int>(tok.value)));
                    expectOperand = false;
                    break;

                case TokenType::LParen:
                    ops_.push_back(Op{tok.type, 0, false, false, true});
                    expectOperand = true;
                    break;

                case TokenType::RParen:
                    while (!ops_.empty() && !ops_.back().lparen) {
                        Op o = ops_.back();
                        ops_.pop_back();
                        fold(o);
                    }
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
                               (ops_.back().prec > p ||
                                (ops_.back().prec == p && !ra))) {
                            Op o = ops_.back();
                            ops_.pop_back();
                            fold(o);
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

        while (!ops_.empty()) {
            Op o = ops_.back();
            ops_.pop_back();
            fold(o);
        }
        if (operands_.size() != 1) throw std::runtime_error("invalid expression");
        return std::move(operands_.back());
    }

private:
    std::vector<Token>  tokens_;
    std::vector<ExprPtr> operands_;
    std::vector<Op>     ops_;
};

}  // namespace

std::unique_ptr<IParser> make_shunting_yard() {
    return std::make_unique<ShuntingYard>();
}

}  // namespace mp
