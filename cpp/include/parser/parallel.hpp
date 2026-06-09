#pragma once

#include <string_view>

#include "parser/ast.hpp"

namespace mp {

// Parse a single expression using up to `threads` threads.
//
// The expression is split at top-level (paren-depth 0) additive operators into
// independent terms; the terms are parsed in parallel and then folded
// left-associatively (which reproduces the sequential result, since '+'/'-' are
// left-associative). Tokenization happens once up front; the worker threads parse
// disjoint token ranges, so there is no redundant lexing.
//
// Falls back to sequential parsing when threads <= 1 or the expression has no
// top-level additive structure (e.g. one big parenthesized/multiplicative term).
// Throws std::runtime_error on a syntax error.
ExprPtr parse_parallel(std::string_view src, unsigned threads);

}  // namespace mp
