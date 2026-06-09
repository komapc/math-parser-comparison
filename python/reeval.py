#!/usr/bin/env python3
"""Re-evaluation benchmark: compile once, evaluate many against changing variables.

The one-shot conclusion ("don't build a tree") inverts here — when you evaluate
the same expression repeatedly with different variable values, building a reusable
compiled form (pointer AST / arena AST / bytecode) amortizes the parse away, and
re-parsing every time (the `reparse-rd` baseline) becomes the slow option.

Reads the shared variable corpus (bench/corpus/vars_n*.txt). Reports compile and
per-eval ns/expr, and the break-even eval count vs re-parsing.
"""
import os
import sys
import threading
import time
from random import Random

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mathparser.lexer import tokenize  # noqa: E402
from mathparser.evaluators import (  # noqa: E402
    rd_parse, eval_ast, TupleBuilder, ArenaBuilder, _MP,
    compile_bytecode, run_bytecode, DirectBuilder,
)

CORPUS = os.path.normpath(os.path.join(HERE, "..", "bench", "corpus"))
SIZES = [10, 100, 1000]
REPS = 5
NUM_VARS = 26


# ---- compilers: each compiles src -> a reusable form, then evals it with vars ---
class AstPtr:
    name = "ast-ptr"
    def compile(self, src):
        return rd_parse(tokenize(src), TupleBuilder(None))   # nested-tuple tree
    def eval(self, tree, vars):
        return eval_ast(tree, vars)


class _Arena:
    def eval(self, compiled, vars):
        builder, root = compiled
        builder.vars = vars
        return builder._walk(root)


class AstArena(_Arena):
    name = "ast-arena"
    def compile(self, src):
        b = ArenaBuilder(None)
        return (b, rd_parse(tokenize(src), b))


class MultipassArena(_Arena):
    name = "multipass-arena"
    def compile(self, src):
        b = ArenaBuilder(None)
        return (b, _MP(tokenize(src), False).parse(b))


class Bytecode:
    name = "bytecode"
    def compile(self, src):
        return compile_bytecode(tokenize(src))
    def eval(self, code, vars):
        return run_bytecode(code, vars)


class ReparseRd:
    name = "reparse-rd"
    def compile(self, src):
        return src                                           # no compiled form
    def eval(self, src, vars):
        b = DirectBuilder(vars)
        return b.result(rd_parse(tokenize(src), b))          # full re-parse each eval


COMPILERS = [AstPtr(), AstArena(), MultipassArena(), Bytecode(), ReparseRd()]


def make_envs(n, seed=0x5EED):
    rng = Random(seed)
    return [[rng.uniform(0.5, 2.0) for _ in range(NUM_VARS)] for _ in range(n)]


def same(a, b):
    import math
    if math.isnan(a) and math.isnan(b):
        return True
    return a == b or abs(a - b) <= 1e-6 * max(1.0, abs(a), abs(b))


def best(reps, f):
    b = float("inf")
    for _ in range(reps):
        t0 = time.perf_counter_ns()
        f()
        b = min(b, time.perf_counter_ns() - t0)
    return b


def run():
    envs = make_envs(8)
    print("== Python: re-evaluation (compile once, evaluate many) ==\n")

    # correctness: every compiler agrees with the first across environments
    bad = 0
    for size in SIZES:
        with open(os.path.join(CORPUS, f"vars_n{size}.txt")) as fh:
            corpus = [l for l in fh.read().splitlines() if l][:200]
        for src in corpus:
            forms = [(c, c.compile(src)) for c in COMPILERS]
            for env in envs:
                ref = forms[0][0].eval(forms[0][1], env)
                for c, comp in forms[1:]:
                    if not same(ref, c.eval(comp, env)):
                        bad += 1
                        break
    print(f"Correctness: {'all agree' if bad == 0 else str(bad) + ' MISMATCHES'}\n")

    print("Variables a-d, values in [0.5, 2.0]")
    print(f"{'strategy':<16}{'leaves':>8}{'exprs':>8}{'compile ns':>14}"
          f"{'per-eval ns':>14}{'break-even':>12}")
    print("-" * 72)

    for size in SIZES:
        with open(os.path.join(CORPUS, f"vars_n{size}.txt")) as fh:
            corpus = [l for l in fh.read().splitlines() if l]
        n = len(corpus)
        rows, reparse_eval = [], None
        for c in COMPILERS:
            compile_ns = best(REPS, lambda: [c.compile(s) for s in corpus]) / n
            forms = [c.compile(s) for s in corpus]

            def do_eval():
                acc, ei = 0.0, 0
                for comp in forms:
                    acc += c.eval(comp, envs[ei])
                    ei = (ei + 1) % len(envs)
                return acc
            eval_ns = best(REPS, do_eval) / n
            rows.append((c.name, compile_ns, eval_ns))
            if c.name == "reparse-rd":
                reparse_eval = eval_ns

        for name, compile_ns, eval_ns in rows:
            denom = reparse_eval - eval_ns
            be = "(baseline)" if name == "reparse-rd" else \
                 ("n/a" if denom <= 0 else f"{compile_ns / denom:.1f}")
            print(f"{name:<16}{size:>8}{n:>8}{compile_ns:>14.1f}{eval_ns:>14.2f}{be:>12}")
        print()
    return 0


def main():
    sys.setrecursionlimit(1_000_000)
    rc = [1]
    threading.stack_size(512 * 1024 * 1024)
    t = threading.Thread(target=lambda: rc.__setitem__(0, run()))
    t.start(); t.join()
    return rc[0]


if __name__ == "__main__":
    sys.exit(main())
