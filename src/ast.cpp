#include "parser/ast.hpp"

#include <cmath>
#include <stdexcept>

namespace mp {

double Number::eval(const double*) const {
    return value;
}

double Variable::eval(const double* vars) const {
    return vars[index];
}

double Unary::eval(const double* vars) const {
    const double v = operand->eval(vars);
    switch (op) {
        case TokenType::Plus:  return +v;
        case TokenType::Minus: return -v;
        default:
            throw std::runtime_error("invalid unary operator");
    }
}

double Binary::eval(const double* vars) const {
    const double l = lhs->eval(vars);
    const double r = rhs->eval(vars);
    switch (op) {
        case TokenType::Plus:  return l + r;
        case TokenType::Minus: return l - r;
        case TokenType::Star:  return l * r;
        case TokenType::Slash: return l / r;
        case TokenType::Caret: return std::pow(l, r);
        default:
            throw std::runtime_error("invalid binary operator");
    }
}

}  // namespace mp
