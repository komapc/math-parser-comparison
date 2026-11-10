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
        // Streaming lexer: shunting-yard reads its input once, left to right.
        Lexer lx(src);
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

        // Two-mode scan: the grammar alternates operand / operator, so each
        // mode dispatches over its own small set of token kinds instead of one
        // switch guarded by an expect-operand flag (the same treatment the
        // fused bottom-up reducer gets; see multipass_reverse_fold.cpp).
        for (;;) {
            // -- operand mode: number, variable, '(' or a prefix sign
            for (;;) {
                const Token tok = lx.next();
                if (tok.type == TokenType::Number) { operands_.push_back(number(tok.value)); break; }
                if (tok.type == TokenType::LParen) {
                    ops_.push_back(Op{tok.type, 0, false, false, true});
                    continue;
                }
                if (tok.type == TokenType::Minus || tok.type == TokenType::Plus) {
                    ops_.push_back(Op{tok.type, 3, true, true, false});
                    continue;
                }
                if (tok.type == TokenType::Ident) { operands_.push_back(variable(static_cast<int>(tok.value))); break; }
                if (tok.type == TokenType::End) throw std::runtime_error("unexpected end of input");
                if (tok.type == TokenType::RParen) throw std::runtime_error("empty parentheses");
                throw std::runtime_error("unexpected operator");
            }
            // -- operator mode: binary op, ')' or end; ')' stays in this mode
            for (;;) {
                const Token tok = lx.next();
                const int p = binPrec(tok.type);
                if (p > 0) {
                    const bool ra = (tok.type == TokenType::Caret);
                    while (!ops_.empty() && !ops_.back().lparen &&
                           (ops_.back().prec > p || (ops_.back().prec == p && !ra))) {
                        Op o = ops_.back(); ops_.pop_back(); fold(o);
                    }
                    ops_.push_back(Op{tok.type, p, ra, false, false});
                    break;
                }
                if (tok.type == TokenType::RParen) {
                    while (!ops_.empty() && !ops_.back().lparen) { Op o = ops_.back(); ops_.pop_back(); fold(o); }
                    if (ops_.empty()) throw std::runtime_error("mismatched parenthesis");
                    ops_.pop_back();
                    continue;
                }
                if (tok.type == TokenType::End) goto done;
                if (tok.type == TokenType::Number) throw std::runtime_error("unexpected number");
                if (tok.type == TokenType::Ident) throw std::runtime_error("unexpected variable");
                throw std::runtime_error("unexpected '('");
            }
        }
    done:

        while (!ops_.empty()) {
            Op o = ops_.back();
            ops_.pop_back();
            fold(o);
        }
        if (operands_.size() != 1) throw std::runtime_error("invalid expression");
        return std::move(operands_.back());
    }

private:
    std::vector<ExprPtr> operands_;
    std::vector<Op>     ops_;
};

}  // namespace

std::unique_ptr<IParser> make_shunting_yard() {
    return std::make_unique<ShuntingYard>();
}

}  // namespace mp
