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

// Fused bottom-up: the two binary levels contract in the materialising sweep
// (two accumulators per paren frame); tree-building and direct-eval forms.
std::unique_ptr<IEvaluator> make_multipass_reverse_fold();
std::unique_ptr<IEvaluator> make_direct_reverse();

// Direct evaluators: compute the value while parsing, no intermediate form.
std::unique_ptr<IEvaluator> make_direct_recursive_descent();
std::unique_ptr<IEvaluator> make_direct_shunting_yard();

// Direct recursive descent with the lexer fused in: reads characters, never
// builds the token array. The control for what the shared tokenizer costs.
std::unique_ptr<IEvaluator> make_direct_scannerless();

// Compile-to-flat-form strategy (one allocation, cache-friendly run).
std::unique_ptr<IEvaluator> make_bytecode();

// Parallel multipass: middle-split + fork-join at 1/2/4/8 threads.
// NOT in all_evaluators() — use single_par_bench for scaling measurements.
std::unique_ptr<IEvaluator> make_multipass_par1();
std::unique_ptr<IEvaluator> make_multipass_par2();
std::unique_ptr<IEvaluator> make_multipass_par4();
std::unique_ptr<IEvaluator> make_multipass_par8();

// Parallel multipass, take two: persistent pool + atomic-free arena + raised
// fork threshold. Same tree as par*, but the fork-join is meant to actually pay.
std::unique_ptr<IEvaluator> make_multipass_pool2();
std::unique_ptr<IEvaluator> make_multipass_pool4();
std::unique_ptr<IEvaluator> make_multipass_pool8();

// Direct-eval fork-join: fuses evaluation into the parallel parse (no tree, no
// node array), pulling the eval walk out of the serial fraction.
std::unique_ptr<IEvaluator> make_multipass_dfork2();
std::unique_ptr<IEvaluator> make_multipass_dfork4();
std::unique_ptr<IEvaluator> make_multipass_dfork8();

// Serial-floor probe: runs only tokenize + buildCandidates (the prologue every
// parallel variant must do before forking), to measure the un-parallelisable floor.
std::unique_ptr<IEvaluator> make_mp_setup_only();

// All evaluators in display order (AST -> direct -> compiled).
std::vector<std::unique_ptr<IEvaluator>> all_evaluators();

// The intra-expression parallel variants (par*/pool*/dfork*). Kept out of
// all_evaluators() so the one-shot benchmarks stay single-threaded, but held
// to the same specification by the correctness suite and the differential
// fuzz (with inputs long enough to actually fork).
std::vector<std::unique_ptr<IEvaluator>> parallel_evaluators();

}  // namespace mp
