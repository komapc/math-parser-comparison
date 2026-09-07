#pragma once

#include <memory>
#include <string_view>

#include "parser/ast.hpp"

namespace mp {

// Interface implemented by the AST-producing parsing strategies (recursive-
// descent, shunting-yard, Pratt, multipass). It is not what the benchmark
// harness drives directly: AstEvaluator (src/evaluators.cpp) wraps an
// IParser to produce an IEvaluator, which is the interface the harness
// actually uses for every strategy — including the ones (e.g. the arena/lean/
// opt multipass variants) that implement IEvaluator directly and never go
// through IParser at all.
class IParser {
public:
    virtual ~IParser() = default;

    // Human-readable strategy name (e.g. "recursive-descent").
    virtual const char* name() const = 0;

    // Parse a full expression into an AST.
    // Throws std::runtime_error on a syntax error.
    virtual ExprPtr parse(std::string_view src) = 0;
};

// Factories for the pointer-AST parsers (wrapped as evaluators in evaluators.cpp).
std::unique_ptr<IParser> make_recursive_descent();
std::unique_ptr<IParser> make_shunting_yard();
std::unique_ptr<IParser> make_pratt();
std::unique_ptr<IParser> make_multipass();

}  // namespace mp
