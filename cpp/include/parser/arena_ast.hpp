#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace mp {

// Arena-allocated AST: every node lives in one contiguous vector and references
// its children by index (not pointer). Building is ~one allocation (the vector,
// reserved from the token count) instead of one per node; eval() is
// allocation-free and walks contiguous memory.
class ArenaAst {
public:
    enum class K : std::uint8_t { Num, Var, Pos, Neg, Add, Sub, Mul, Div, Pow };

    struct Node {
        K      kind;
        int    a = -1;       // child index (unary operand / binary lhs)
        int    b = -1;       // child index (binary rhs)
        int    var = 0;      // Var: variable index
        double value = 0.0;  // Num: literal value
    };

    ArenaAst() = default;

    // Build via recursive descent. Throws std::runtime_error on a syntax error.
    static ArenaAst parse(std::string_view src);

    // Re-build in place, reusing this arena's node buffer capacity — the
    // steady-state entry point, so repeated evals are allocation-comparable
    // with the multipass family's reused member buffers.
    void reparse(std::string_view src);

    // Adopt a pre-built node vector + root index (used by alternative parsers).
    static ArenaAst adopt(std::vector<Node> nodes, int root);

    // Evaluate with the variable environment (nullptr for constant expressions).
    double eval(const double* vars) const { return evalNode(root_, vars); }

    std::size_t nodeCount() const { return nodes_.size(); }

private:
    std::vector<Node> nodes_;
    int root_ = -1;

    double evalNode(int i, const double* vars) const;
};

}  // namespace mp
