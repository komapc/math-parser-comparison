#pragma once

#include <memory>
#include <utility>

#include "parser/token.hpp"

namespace mp {

// Common AST shared by all AST strategies, so results can be compared
// structurally and evaluated through a single code path.
//
// eval() takes the variable environment: a pointer to an array indexed by
// variable id (a-z -> 0-25). It may be nullptr for constant expressions, which
// never contain a Variable node.
struct Expr {
    virtual ~Expr() = default;
    virtual double eval(const double* vars) const = 0;
};

using ExprPtr = std::unique_ptr<Expr>;

struct Number final : Expr {
    double value;
    explicit Number(double v) : value(v) {}
    double eval(const double* vars) const override;
};

struct Variable final : Expr {
    int index;
    explicit Variable(int index) : index(index) {}
    double eval(const double* vars) const override;
};

struct Unary final : Expr {
    TokenType op;       // Plus or Minus
    ExprPtr   operand;
    Unary(TokenType op, ExprPtr operand)
        : op(op), operand(std::move(operand)) {}
    double eval(const double* vars) const override;
};

struct Binary final : Expr {
    TokenType op;
    ExprPtr   lhs;
    ExprPtr   rhs;
    Binary(TokenType op, ExprPtr lhs, ExprPtr rhs)
        : op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
    double eval(const double* vars) const override;
};

// Convenience constructors.
inline ExprPtr number(double v) { return std::make_unique<Number>(v); }
inline ExprPtr variable(int index) { return std::make_unique<Variable>(index); }
inline ExprPtr unary(TokenType op, ExprPtr e) {
    return std::make_unique<Unary>(op, std::move(e));
}
inline ExprPtr binary(TokenType op, ExprPtr l, ExprPtr r) {
    return std::make_unique<Binary>(op, std::move(l), std::move(r));
}

}  // namespace mp
