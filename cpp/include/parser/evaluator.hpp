#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "parser/arena_ast.hpp"

namespace mp {

// Uniform one-shot interface: source text -> numeric value, in a single call.
// Every strategy implements this, so they can be compared apples-to-apples.
// vars: variable environment indexed a=0..z=25; nullptr for constant expressions.
class IEvaluator {
public:
    virtual ~IEvaluator() = default;
    virtual const char* name() const = 0;
    virtual double eval(std::string_view src, const double* vars = nullptr) = 0;
};

// AST-building strategies, wrapped as one-shot evaluators (parse + tree-walk).
std::unique_ptr<IEvaluator> make_ast_recursive_descent();
std::unique_ptr<IEvaluator> make_ast_shunting_yard();
std::unique_ptr<IEvaluator> make_ast_pratt();

// Arena-allocated AST (one contiguous buffer instead of per-node allocation).
std::unique_ptr<IEvaluator> make_ast_arena();

// Divide-and-conquer: recursively split on the lowest-precedence operator.
std::unique_ptr<IEvaluator> make_ast_multipass();

// Same algorithm, arena-allocated (no per-node heap allocation).
std::unique_ptr<IEvaluator> make_ast_multipass_arena();

// Parse with multipass-arena and return the arena AST (for reeval / ICompiler use).
ArenaAst multipass_arena_parse(std::string_view src);

// Direct-eval D&C variant (no AST, evaluate inline).
std::unique_ptr<IEvaluator> make_direct_mp();

// Optimised multipass (sparse-table RMQ + paren pre-index + BFS).
std::unique_ptr<IEvaluator> make_multipass_bfs();

// Reverse (bottom-up) multipass: reduce innermost/highest-precedence first.
std::unique_ptr<IEvaluator> make_multipass_reverse();

// Direct evaluators: compute the value while parsing, no intermediate form.
std::unique_ptr<IEvaluator> make_direct_recursive_descent();
std::unique_ptr<IEvaluator> make_direct_shunting_yard();

// Compile-to-flat-form strategy (one allocation, cache-friendly run).
std::unique_ptr<IEvaluator> make_bytecode();

// Parallel multipass: middle-split + fork-join at 1/2/4/8 threads.
// NOT in all_evaluators() — use single_par_bench for scaling measurements.
std::unique_ptr<IEvaluator> make_multipass_par1();
std::unique_ptr<IEvaluator> make_multipass_par2();
std::unique_ptr<IEvaluator> make_multipass_par4();
std::unique_ptr<IEvaluator> make_multipass_par8();

// All evaluators in display order (AST -> direct -> compiled).
std::vector<std::unique_ptr<IEvaluator>> all_evaluators();

}  // namespace mp
