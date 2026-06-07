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
        const std::vector<Token> tokens = tokenize(src);
        std::vector<ExprPtr> operands;
        std::vector<Op> ops;

        auto fold = [&](const Op& op) {
            if (op.lparen) throw std::runtime_error("mismatched parenthesis");
            if (op.unary) {
                if (operands.empty()) throw std::runtime_error("missing operand");
                ExprPtr e = std::move(operands.back());
                operands.pop_back();
                operands.push_back(unary(op.type, std::move(e)));
            } else {
                if (operands.size() < 2) throw std::runtime_error("missing operand");
                ExprPtr r = std::move(operands.back());
                operands.pop_back();
                ExprPtr l = std::move(operands.back());
                operands.pop_back();
                operands.push_back(binary(op.type, std::move(l), std::move(r)));
            }
        };

        bool expectOperand = true;
        for (const Token& tok : tokens) {
            switch (tok.type) {
                case TokenType::Number:
                    if (!expectOperand) throw std::runtime_error("unexpected number");
                    operands.push_back(number(tok.value));
                    expectOperand = false;
                    break;

                case TokenType::Ident:
                    if (!expectOperand) throw std::runtime_error("unexpected variable");
                    operands.push_back(variable(static_cast<int>(tok.value)));
                    expectOperand = false;
                    break;

                case TokenType::LParen:
                    ops.push_back(Op{tok.type, 0, false, false, true});
                    expectOperand = true;
                    break;

                case TokenType::RParen:
                    while (!ops.empty() && !ops.back().lparen) {
                        Op o = ops.back();
                        ops.pop_back();
                        fold(o);
                    }
                    if (ops.empty()) throw std::runtime_error("mismatched parenthesis");
                    ops.pop_back();  // discard the matching '('
                    expectOperand = false;
                    break;

                case TokenType::Plus:
                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Caret:
                    if (expectOperand) {
                        // Operator in operand position => prefix unary (+/- only).
                        if (tok.type != TokenType::Plus && tok.type != TokenType::Minus) {
                            throw std::runtime_error("unexpected operator");
                        }
                        ops.push_back(Op{tok.type, 3, true, true, false});
                        // still expecting an operand
                    } else {
                        const int p = binPrec(tok.type);
                        const bool ra = (tok.type == TokenType::Caret);
                        while (!ops.empty() && !ops.back().lparen &&
                               (ops.back().prec > p ||
                                (ops.back().prec == p && !ra))) {
                            Op o = ops.back();
                            ops.pop_back();
                            fold(o);
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

        while (!ops.empty()) {
            Op o = ops.back();
            ops.pop_back();
            fold(o);  // throws on a leftover '('
        }
        if (operands.size() != 1) throw std::runtime_error("invalid expression");
        return std::move(operands.back());
    }
};

}  // namespace

std::unique_ptr<IParser> make_shunting_yard() {
    return std::make_unique<ShuntingYard>();
}

}  // namespace mp
