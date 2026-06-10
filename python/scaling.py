#!/usr/bin/env python3
"""Batch-throughput scaling for Python — and a live demonstration of the GIL.

Runs a representative set of strategies over the shared corpus split across W
workers, with W = 1, 2, 4, using BOTH a process pool and a thread pool. The
point: CPU-bound work does not scale with *threads* in CPython (the GIL
serialises bytecode) but does scale with *processes*. Speedups are relative to
that pool's own W=1.

Uses the n=100 corpus (shallow trees — no deep-recursion stack issues in workers).
"""
import os
import sys
import time
from multiprocessing import Pool
from multiprocessing.dummy import Pool as ThreadPool

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.normpath(os.path.join(HERE, "..", "bench", "corpus", "n100.txt"))
SIZE_N = 100
WORKERS = [1, 2, 4]

# The eight tree-building strategies, so the W=1 vs W=4 columns show whether the
# AST-builder *ranking* is core-count-invariant. (Process-pool spawn overhead
# makes this a demonstration, not a precise bench — trust the order, not digits.)
STRATEGIES = [
    "ast-recursive-descent",   # pointer AST, per-node allocation
    "ast-shunting-yard",       # pointer AST
    "ast-pratt",               # pointer AST
    "ast-arena",               # arena AST, one allocation
    "multipass",               # D&C, pointer AST
    "multipass-arena",         # D&C, arena AST
    "multipass-bfs",           # D&C + sparse-table RMQ
    "multipass-reverse",       # D&C, bottom-up
]

_EV = None  # per-worker evaluator


def _init(name):
    global _EV
    sys.path.insert(0, HERE)
    from mathparser import all_evaluators
    sys.setrecursionlimit(200_000)
    _EV = next(e for e in all_evaluators() if e.name == name)


def _work(chunk):
    return sum(_EV.eval(e) for e in chunk)


def chunks(work, w):
    return [work[i::w] for i in range(w)]


def time_pool(pool_factory, name, work, w, reps=1):
    best = float("inf")
    pool = pool_factory(w, initializer=_init, initargs=(name,))
    try:
        parts = chunks(work, w)
        for _ in range(reps):
            t0 = time.perf_counter()
            pool.map(_work, parts)
            best = min(best, time.perf_counter() - t0)
    finally:
        pool.close()
        pool.join()
    return best


def main():
    with open(CORPUS) as f:
        corpus = [ln for ln in f.read().splitlines() if ln]
    work = corpus                          # n100 corpus = 1000 exprs, ~100k leaves
    leaves = len(work) * SIZE_N

    print("== Python: batch-throughput scaling ==")
    print(f"cpu_count={os.cpu_count()}  corpus=n{SIZE_N} ({len(work)} exprs, "
          f"throughput Mleaf/s — higher is better)\n")
    print(f"{'strategy':<26}{'pool':<8}" + "".join(f"{'W=' + str(w):>9}" for w in WORKERS)
          + f"{'speedup@4':>11}")
    print("-" * (26 + 8 + 9 * len(WORKERS) + 11))

    for name in STRATEGIES:
        for label, factory in (("process", Pool), ("thread", ThreadPool)):
            tput = []  # Mleaf/s at each W
            row = f"{name:<26}{label:<8}"
            for w in WORKERS:
                t = time_pool(factory, name, work, w)
                tput.append(leaves / t / 1e6)
                row += f"{leaves / t / 1e6:>9.2f}"
            row += f"{tput[-1] / tput[0]:>10.2f}x"  # speedup@4 = throughput ratio
            print(row)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
