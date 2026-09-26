#!/usr/bin/env python3
"""Benchmark the fifteen strategies on the shared corpora (bench/corpus/).

First cross-checks that every strategy agrees with the first on the random
corpus (the real correctness test), then prints ns/leaf per strategy.

Deep n=10000 expressions recurse past CPython's default C stack, so the work
runs in a thread with a large stack.
"""
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mathparser import all_evaluators  # noqa: E402

CORPUS = os.path.normpath(os.path.join(HERE, "..", "bench", "corpus"))
SIZES = [10, 100, 1000, 10000]
# every cell is a best-of-N; N=5 at every size, like the n<=1000 cells of the
# compiled languages (n=1000 at N=3 left a tie cell resting on too few draws)
REPS = {10: 5, 100: 5, 1000: 5, 10000: 5}


def load(size):
    path = os.path.join(CORPUS, f"n{size}.txt")
    with open(path) as f:
        return [ln for ln in f.read().splitlines() if ln]


def same(a, b):
    import math
    if math.isnan(a) and math.isnan(b):
        return True
    if a == b:
        return True
    if math.isinf(a) or math.isinf(b):
        return False
    return abs(a - b) <= 1e-6 * max(1.0, abs(a), abs(b))


def correctness(corpora):
    evs = all_evaluators()
    total = bad = 0
    for size, corpus in corpora.items():
        # cross-check a capped subset per size to keep it quick
        subset = corpus[: min(len(corpus), 500)]
        for expr in subset:
            total += 1
            ref = evs[0].eval(expr)
            for ev in evs[1:]:
                if not same(ref, ev.eval(expr)):
                    bad += 1
                    if bad <= 5:
                        print(f"  MISMATCH n{size} {ev.name}: {expr[:60]}... "
                              f"{evs[0].name}={ref:g} {ev.name}={ev.eval(expr):g}")
                    break
    print(f"Correctness: {total - bad}/{total} expressions agree"
          f"{'  <-- FAILURES' if bad else ''}\n")
    return bad == 0


def time_ns(ev, corpus):
    t0 = time.perf_counter_ns()
    acc = 0.0
    for e in corpus:
        acc += ev.eval(e)
    dt = time.perf_counter_ns() - t0
    if acc == 12345.6789:  # defeat any clever optimizer; never true
        print(acc)
    return dt


def run():
    corpora = {s: load(s) for s in SIZES}
    print("== Python: math-expression evaluator comparison ==\n")
    if not correctness(corpora):
        return 1

    print("One-shot ns/leaf on shared corpora:")
    header = f"{'strategy':<26}" + "".join(f"{'n=' + str(s):>12}" for s in SIZES)
    print(header)
    print("-" * len(header))
    # Interleaved: each rep times every strategy once, round-robin, so slow
    # drift on the runner (clock, thermals, noisy neighbours) lands on all
    # strategies alike instead of on whichever ran late. Best-of-reps per cell.
    evs = all_evaluators()
    best = [[float("inf")] * len(SIZES) for _ in evs]
    for i, s in enumerate(SIZES):
        for _ in range(REPS[s]):
            for k, ev in enumerate(evs):
                best[k][i] = min(best[k][i], time_ns(ev, corpora[s]))

    for k, ev in enumerate(evs):
        row = f"{ev.name:<26}"
        for i, s in enumerate(SIZES):
            row += f"{best[k][i] / len(corpora[s]) / s:>12.1f}"
        print(row)
    return 0


def main():
    sys.setrecursionlimit(1_000_000)
    rc = [1]

    def target():
        rc[0] = run()

    threading.stack_size(512 * 1024 * 1024)
    t = threading.Thread(target=target)
    t.start()
    t.join()
    return rc[0]


if __name__ == "__main__":
    sys.exit(main())
