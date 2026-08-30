#include "parser/evaluator.hpp"
#include "parser/arena_ast.hpp"
#include "parser/parser.hpp"
#include "parser/reeval.hpp"

#include <utility>

namespace mp {
namespace {

// The multipass family treats null vars as all-zero with a per-leaf branch;
// the classic builders keep their leaf code branch-free and honour the same
// contract by substituting this table once per eval.
constexpr double kZeroVars[kNumVars]{};

// Arena AST wrapped as a one-shot evaluator (constant expression -> value).
// The arena is a member so its node-buffer capacity is reused across evals —
// the same warm-buffer discipline the multipass family gets from its member
// vectors, keeping the ast-arena vs multipass-reverse comparison fair.
class ArenaEvaluator final : public IEvaluator {
public:
    const char* name() const override { return "ast-arena"; }
    double eval(std::string_view src, const double* vars = nullptr) override {
        ast_.reparse(src);
        return ast_.eval(vars ? vars : kZeroVars);
    }

private:
    ArenaAst ast_;
};

class AstEvaluator final : public IEvaluator {
public:
    AstEvaluator(const char* label, std::unique_ptr<IParser> parser)
        : label_(label), parser_(std::move(parser)) {}

    const char* name() const override { return label_; }
    double eval(std::string_view src, const double* vars = nullptr) override {
        return parser_->parse(src)->eval(vars ? vars : kZeroVars);
    }

private:
    const char* label_;
    std::unique_ptr<IParser> parser_;
};

}  // namespace

std::unique_ptr<IEvaluator> make_ast_recursive_descent() {
    return std::make_unique<AstEvaluator>("ast-recursive-descent", make_recursive_descent());
}
std::unique_ptr<IEvaluator> make_ast_shunting_yard() {
    return std::make_unique<AstEvaluator>("ast-shunting-yard", make_shunting_yard());
}
std::unique_ptr<IEvaluator> make_ast_pratt() {
    return std::make_unique<AstEvaluator>("ast-pratt", make_pratt());
}
std::unique_ptr<IEvaluator> make_ast_arena() {
    return std::make_unique<ArenaEvaluator>();
}
std::unique_ptr<IEvaluator> make_ast_multipass() {
    return std::make_unique<AstEvaluator>("multipass", make_multipass());
}
// make_ast_multipass_arena() is defined in multipass_arena.cpp

std::vector<std::unique_ptr<IEvaluator>> all_evaluators() {
    std::vector<std::unique_ptr<IEvaluator>> v;
    v.push_back(make_ast_recursive_descent());
    v.push_back(make_ast_shunting_yard());
    v.push_back(make_ast_pratt());
    v.push_back(make_ast_arena());
    v.push_back(make_ast_multipass());
    v.push_back(make_ast_multipass_arena());
    v.push_back(make_direct_mp());
    v.push_back(make_multipass_bfs());
    v.push_back(make_multipass_reverse());
    v.push_back(make_direct_recursive_descent());
    v.push_back(make_direct_shunting_yard());
    v.push_back(make_bytecode());
    return v;
}

std::vector<std::unique_ptr<IEvaluator>> parallel_evaluators() {
    std::vector<std::unique_ptr<IEvaluator>> v;
    v.push_back(make_multipass_par1());
    v.push_back(make_multipass_par2());
    v.push_back(make_multipass_par4());
    v.push_back(make_multipass_par8());
    v.push_back(make_multipass_pool2());
    v.push_back(make_multipass_pool4());
    v.push_back(make_multipass_pool8());
    v.push_back(make_multipass_dfork2());
    v.push_back(make_multipass_dfork4());
    v.push_back(make_multipass_dfork8());
    return v;
}

}  // namespace mp
