#pragma once

#include <memory>
#include <string_view>
#include <vector>

namespace mp {

// Uniform one-shot interface: source text -> numeric value, in a single call.
// Every strategy implements this, so they can be compared apples-to-apples.
// (AST builders parse then walk the tree; direct evaluators compute inline;
// RPN/bytecode compile to a flat form then run it -- all within eval().)
class IEvaluator {
public:
    virtual ~IEvaluator() = default;
    virtual const char* name() const = 0;
    virtual double eval(std::string_view src) = 0;
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

// Direct-eval multipass variants (no AST, evaluate inline).
std::unique_ptr<IEvaluator> make_direct_mp();
std::unique_ptr<IEvaluator> make_direct_mp_simd();
std::unique_ptr<IEvaluator> make_direct_mp_full();

// Optimised multipass variants (paren pre-indexing + sparse RMQ + Cartesian skeleton + BFS).
std::unique_ptr<IEvaluator> make_multipass_paren_idx();
std::unique_ptr<IEvaluator> make_multipass_sparse();
std::unique_ptr<IEvaluator> make_multipass_cartesian();
std::unique_ptr<IEvaluator> make_multipass_bfs();

// Direct evaluators: compute the value while parsing, no intermediate form.
std::unique_ptr<IEvaluator> make_direct_recursive_descent();
std::unique_ptr<IEvaluator> make_direct_shunting_yard();

// Compile-to-flat-form strategies (one allocation, cache-friendly run).
std::unique_ptr<IEvaluator> make_rpn();
std::unique_ptr<IEvaluator> make_bytecode();

// All evaluators in display order (AST -> direct -> compiled).
std::vector<std::unique_ptr<IEvaluator>> all_evaluators();

}  // namespace mp
