#include "parser/reeval.hpp"

#include "parser/arena_ast.hpp"
#include "parser/evaluator.hpp"
#include "parser/lexer.hpp"
#include "parser/parser.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mp {
namespace {

// ---- shared shunting-yard precedence (used by the RPN and bytecode compilers)
struct Op {
    TokenType type;
    int  prec;
    bool rightAssoc;
    bool unary;
    bool lparen;
};

int binPrec(TokenType t) {
    switch (t) {
        case TokenType::Plus:
        case TokenType::Minus: return 1;
        case TokenType::Star:
        case TokenType::Slash: return 2;
        case TokenType::Caret: return 4;
        default:               return -1;
    }
}

// ============================================================ reparse baseline
// In one sentence: the no-compile baseline — re-lex, re-parse and walk a fresh
// AST on every single evaluation.
// This is what having no reusable compiled form costs.
class ReparseCompiled final : public ICompiledExpr {
public:
    explicit ReparseCompiled(std::string_view src)
        : src_(src), parser_(make_recursive_descent()) {}
    double eval(const double* vars) const override {
        return parser_->parse(src_)->eval(vars);
    }
private:
    std::string              src_;
    std::unique_ptr<IParser> parser_;  // reused, but parse() still allocates an AST
};

class ReparseCompiler final : public ICompiler {
public:
    const char* name() const override { return "reparse-rd"; }
    std::unique_ptr<ICompiledExpr> compile(std::string_view src) override {
        return std::make_unique<ReparseCompiled>(src);
    }
};

// ================================================================ AST (unique_ptr)
// In one sentence: parse to a pointer-linked AST once, then walk that tree on
// each evaluation.
class AstPtrCompiled final : public ICompiledExpr {
public:
    explicit AstPtrCompiled(ExprPtr ast) : ast_(std::move(ast)) {}
    double eval(const double* vars) const override { return ast_->eval(vars); }
private:
    ExprPtr ast_;
};

class AstPtrCompiler final : public ICompiler {
public:
    const char* name() const override { return "ast-ptr"; }
    std::unique_ptr<ICompiledExpr> compile(std::string_view src) override {
        return std::make_unique<AstPtrCompiled>(parser_->parse(src));
    }
private:
    std::unique_ptr<IParser> parser_ = make_recursive_descent();
};

// ===================================================================== AST (arena)
// In one sentence: parse to a flat arena AST once, then walk its contiguous nodes
// on each evaluation.
class ArenaCompiled final : public ICompiledExpr {
public:
    explicit ArenaCompiled(ArenaAst ast) : ast_(std::move(ast)) {}
    double eval(const double* vars) const override { return ast_.eval(vars); }
private:
    ArenaAst ast_;
};

class ArenaCompiler final : public ICompiler {
public:
    const char* name() const override { return "ast-arena"; }
    std::unique_ptr<ICompiledExpr> compile(std::string_view src) override {
        return std::make_unique<ArenaCompiled>(ArenaAst::parse(src));
    }
};

// ================================================================ multipass-arena
// In one sentence: divide-and-conquer arena AST — same tree as ast-arena but built
// top-down; compile cost is higher (~×2.2 vs ×1.3) but the structure uniquely
// supports incremental re-parsing and fork-join parallelism.
class MultipassArenaCompiler final : public ICompiler {
public:
    const char* name() const override { return "multipass-arena"; }
    std::unique_ptr<ICompiledExpr> compile(std::string_view src) override {
        return std::make_unique<ArenaCompiled>(multipass_arena_parse(src));
    }
};

// ============================================================================ RPN
// In one sentence: compile to postfix once, then re-run that flat sequence on a
// reused (allocation-free) value stack each evaluation.
enum class RKind { Num, Var, Add, Sub, Mul, Div, Pow, Neg };

struct RTok {
    RKind  kind;
    double value;  // Num: literal, Var: variable index
};

RKind binRKind(TokenType t) {
    switch (t) {
        case TokenType::Plus:  return RKind::Add;
        case TokenType::Minus: return RKind::Sub;
        case TokenType::Star:  return RKind::Mul;
        case TokenType::Slash: return RKind::Div;
        case TokenType::Caret: return RKind::Pow;
        default:               throw std::runtime_error("invalid operator");
    }
}

std::vector<RTok> compileRpn(std::string_view src) {
    const std::vector<Token> tokens = tokenize(src);
    std::vector<RTok> out;
    std::vector<Op> ops;

    auto emit = [&](const Op& op) {
        if (op.lparen) throw std::runtime_error("mismatched parenthesis");
        if (op.unary) {
            if (op.type == TokenType::Minus) out.push_back({RKind::Neg, 0.0});
        } else {
            out.push_back({binRKind(op.type), 0.0});
        }
    };

    bool expectOperand = true;
    for (const Token& tok : tokens) {
        switch (tok.type) {
            case TokenType::Number:
                if (!expectOperand) throw std::runtime_error("unexpected number");
                out.push_back({RKind::Num, tok.value});
                expectOperand = false;
                break;
            case TokenType::Ident:
                if (!expectOperand) throw std::runtime_error("unexpected variable");
                out.push_back({RKind::Var, tok.value});
                expectOperand = false;
                break;
            case TokenType::LParen:
                ops.push_back(Op{tok.type, 0, false, false, true});
                expectOperand = true;
                break;
            case TokenType::RParen:
                while (!ops.empty() && !ops.back().lparen) { Op o = ops.back(); ops.pop_back(); emit(o); }
                if (ops.empty()) throw std::runtime_error("mismatched parenthesis");
                ops.pop_back();
                expectOperand = false;
                break;
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Caret:
                if (expectOperand) {
                    if (tok.type != TokenType::Plus && tok.type != TokenType::Minus)
                        throw std::runtime_error("unexpected operator");
                    ops.push_back(Op{tok.type, 3, true, true, false});
                } else {
                    const int p = binPrec(tok.type);
                    const bool ra = (tok.type == TokenType::Caret);
                    while (!ops.empty() && !ops.back().lparen &&
                           (ops.back().prec > p || (ops.back().prec == p && !ra))) {
                        Op o = ops.back(); ops.pop_back(); emit(o);
                    }
                    ops.push_back(Op{tok.type, p, ra, false, false});
                    expectOperand = true;
                }
                break;
            case TokenType::End:
                if (expectOperand) throw std::runtime_error("unexpected end of input");
                break;
        }
    }
    while (!ops.empty()) { Op o = ops.back(); ops.pop_back(); emit(o); }
    if (out.empty()) throw std::runtime_error("invalid expression");
    return out;
}

class RpnCompiled final : public ICompiledExpr {
public:
    explicit RpnCompiled(std::vector<RTok> code) : code_(std::move(code)) {
        stack_.reserve(code_.size() + 1);  // reserve once; eval reuses it
    }
    double eval(const double* vars) const override {
        stack_.clear();
        for (const RTok& t : code_) {
            switch (t.kind) {
                case RKind::Num: stack_.push_back(t.value); break;
                case RKind::Var: stack_.push_back(vars[static_cast<int>(t.value)]); break;
                case RKind::Neg: stack_.back() = -stack_.back(); break;
                default: {
                    const double r = stack_.back(); stack_.pop_back();
                    double& l = stack_.back();
                    switch (t.kind) {
                        case RKind::Add: l = l + r; break;
                        case RKind::Sub: l = l - r; break;
                        case RKind::Mul: l = l * r; break;
                        case RKind::Div: l = l / r; break;
                        case RKind::Pow: l = std::pow(l, r); break;
                        default: break;
                    }
                }
            }
        }
        return stack_.back();
    }
private:
    std::vector<RTok>           code_;
    mutable std::vector<double> stack_;
};

class RpnCompiler final : public ICompiler {
public:
    const char* name() const override { return "rpn"; }
    std::unique_ptr<ICompiledExpr> compile(std::string_view src) override {
        return std::make_unique<RpnCompiled>(compileRpn(src));
    }
};

// ======================================================================= bytecode
// In one sentence: compile to a bytecode opcode stream once, then re-run it on a
// reused (allocation-free) stack VM each evaluation.
enum class Bc : std::uint8_t { Push, Load, Add, Sub, Mul, Div, Pow, Neg };

Bc binBc(TokenType t) {
    switch (t) {
        case TokenType::Plus:  return Bc::Add;
        case TokenType::Minus: return Bc::Sub;
        case TokenType::Star:  return Bc::Mul;
        case TokenType::Slash: return Bc::Div;
        case TokenType::Caret: return Bc::Pow;
        default:               throw std::runtime_error("invalid operator");
    }
}

struct Program {
    std::vector<std::uint8_t> code;
    std::vector<double>       consts;
    std::vector<int>          varidx;
};

Program compileBc(std::string_view src) {
    const std::vector<Token> tokens = tokenize(src);
    Program p;
    std::vector<Op> ops;

    auto emit = [&](const Op& op) {
        if (op.lparen) throw std::runtime_error("mismatched parenthesis");
        if (op.unary) {
            if (op.type == TokenType::Minus)
                p.code.push_back(static_cast<std::uint8_t>(Bc::Neg));
        } else {
            p.code.push_back(static_cast<std::uint8_t>(binBc(op.type)));
        }
    };

    bool expectOperand = true;
    for (const Token& tok : tokens) {
        switch (tok.type) {
            case TokenType::Number:
                if (!expectOperand) throw std::runtime_error("unexpected number");
                p.code.push_back(static_cast<std::uint8_t>(Bc::Push));
                p.consts.push_back(tok.value);
                expectOperand = false;
                break;
            case TokenType::Ident:
                if (!expectOperand) throw std::runtime_error("unexpected variable");
                p.code.push_back(static_cast<std::uint8_t>(Bc::Load));
                p.varidx.push_back(static_cast<int>(tok.value));
                expectOperand = false;
                break;
            case TokenType::LParen:
                ops.push_back(Op{tok.type, 0, false, false, true});
                expectOperand = true;
                break;
            case TokenType::RParen:
                while (!ops.empty() && !ops.back().lparen) { Op o = ops.back(); ops.pop_back(); emit(o); }
                if (ops.empty()) throw std::runtime_error("mismatched parenthesis");
                ops.pop_back();
                expectOperand = false;
                break;
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Caret:
                if (expectOperand) {
                    if (tok.type != TokenType::Plus && tok.type != TokenType::Minus)
                        throw std::runtime_error("unexpected operator");
                    ops.push_back(Op{tok.type, 3, true, true, false});
                } else {
                    const int pr = binPrec(tok.type);
                    const bool ra = (tok.type == TokenType::Caret);
                    while (!ops.empty() && !ops.back().lparen &&
                           (ops.back().prec > pr || (ops.back().prec == pr && !ra))) {
                        Op o = ops.back(); ops.pop_back(); emit(o);
                    }
                    ops.push_back(Op{tok.type, pr, ra, false, false});
                    expectOperand = true;
                }
                break;
            case TokenType::End:
                if (expectOperand) throw std::runtime_error("unexpected end of input");
                break;
        }
    }
    while (!ops.empty()) { Op o = ops.back(); ops.pop_back(); emit(o); }
    if (p.code.empty()) throw std::runtime_error("invalid expression");
    return p;
}

class BytecodeCompiled final : public ICompiledExpr {
public:
    explicit BytecodeCompiled(Program p) : p_(std::move(p)) {
        stack_.reserve(p_.code.size() + 1);
    }
    double eval(const double* vars) const override {
        stack_.clear();
        std::size_t ci = 0, vi = 0;
        for (const std::uint8_t opc : p_.code) {
            switch (static_cast<Bc>(opc)) {
                case Bc::Push: stack_.push_back(p_.consts[ci++]); break;
                case Bc::Load: stack_.push_back(vars[p_.varidx[vi++]]); break;
                case Bc::Neg:  stack_.back() = -stack_.back(); break;
                default: {
                    const double r = stack_.back(); stack_.pop_back();
                    double& l = stack_.back();
                    switch (static_cast<Bc>(opc)) {
                        case Bc::Add: l = l + r; break;
                        case Bc::Sub: l = l - r; break;
                        case Bc::Mul: l = l * r; break;
                        case Bc::Div: l = l / r; break;
                        case Bc::Pow: l = std::pow(l, r); break;
                        default: break;
                    }
                }
            }
        }
        return stack_.back();
    }
private:
    Program                     p_;
    mutable std::vector<double> stack_;
};

class BytecodeCompiler final : public ICompiler {
public:
    const char* name() const override { return "bytecode"; }
    std::unique_ptr<ICompiledExpr> compile(std::string_view src) override {
        return std::make_unique<BytecodeCompiled>(compileBc(src));
    }
};

}  // namespace

std::vector<std::unique_ptr<ICompiler>> all_compilers() {
    std::vector<std::unique_ptr<ICompiler>> v;
    v.push_back(std::make_unique<AstPtrCompiler>());
    v.push_back(std::make_unique<ArenaCompiler>());
    v.push_back(std::make_unique<MultipassArenaCompiler>());
    v.push_back(std::make_unique<RpnCompiler>());
    v.push_back(std::make_unique<BytecodeCompiler>());
    v.push_back(std::make_unique<ReparseCompiler>());
    return v;
}

}  // namespace mp
