#pragma once

#include <memory>
#include <string_view>

#include "parser/ast.hpp"

namespace mp {

// Uniform interface implemented by each parsing strategy. The benchmark
// harness drives strategies exclusively through this interface.
class IParser {
public:
    virtual ~IParser() = default;

    // Human-readable strategy name (e.g. "recursive-descent").
    virtual const char* name() const = 0;

    // Parse a full expression into an AST.
    // Throws std::runtime_error on a syntax error.
    virtual ExprPtr parse(std::string_view src) = 0;
};

// Factories for the three strategies under comparison.
std::unique_ptr<IParser> make_recursive_descent();
std::unique_ptr<IParser> make_shunting_yard();
std::unique_ptr<IParser> make_pratt();

}  // namespace mp
