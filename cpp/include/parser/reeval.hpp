#pragma once

#include <memory>
#include <string_view>
#include <vector>

namespace mp {

// "Compile once, evaluate many" comparison. A compiler turns source into a
// reusable compiled form; that form is then evaluated repeatedly against a
// variable environment (a-z -> index 0..25). Crucially, eval() of the compiled
// forms is allocation-free per call -- the only strategy that re-allocates each
// call is the reparse baseline, which is the entire point of comparing to it.
constexpr int kNumVars = 26;

class ICompiledExpr {
public:
    virtual ~ICompiledExpr() = default;
    virtual double eval(const double* vars) const = 0;
};

class ICompiler {
public:
    virtual ~ICompiler() = default;
    virtual const char* name() const = 0;
    virtual std::unique_ptr<ICompiledExpr> compile(std::string_view src) = 0;
};

// All compilers in display order (compiled forms, then the reparse baseline).
std::vector<std::unique_ptr<ICompiler>> all_compilers();

}  // namespace mp
