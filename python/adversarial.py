#!/usr/bin/env python3
"""Adversarial (structured, non-random) inputs that separate the multipass family.

Two flat chains, no parentheses (mirrors cpp/bench/adversarial_bench.cpp):

  powchain  3^2 * 2^2 / 2^2 * 3^3 / 3^3 ...   alternating-precedence (^ vs */)
  sumchain  1 + 2 - 3 + 4 ...                 single-precedence (control)

On powchain the top-down splitters degenerate: the candidate list mixes prec 2
and prec 4, so the flat-chain fold never applies and the linear right-to-left
split scan has no prec-1 early exit — every split rescans its whole range:
Theta(n^2) (multipass, multipass-arena). The sparse-table RMQ (multipass-bfs)
answers splits in O(1) — the one input family where its precompute pays off.
Bottom-up multipass-reverse reduces each level in one pass regardless: Theta(n).
sumchain is the control where the whole family stays linear.
"""
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mathparser import all_evaluators  # noqa: E402

sys.setrecursionlimit(200_000)

SIZES = [64, 256, 1024]
REPS = 3


def pow_chain(m):
    # adjacent factor pairs are identical, so the value stays exactly 9.0
    parts = ["3 ^ 2"]
    for i in range(1, m):
        k = (i - 1) // 2
        parts.append(" * " if i % 2 else " / ")
        parts.append(f"{2 + k % 8} ^ {2 + k % 3}")
    return "".join(parts)


def sum_chain(m):
    parts = ["1"]
    for i in range(1, m):
        parts.append(" + " if i % 2 else " - ")
        parts.append(str(1 + i % 9))
    return "".join(parts)


def run_shape(title, gen, leaves_per_factor):
    print(f"-- {title} --")
    print(f"{'strategy':<26}" + "".join(f"{'m=' + str(m):>12}" for m in SIZES)
          + "   (ns/leaf; flat = linear, ~4x/col = quadratic)")
    exprs = [gen(m) for m in SIZES]
    evs = all_evaluators()

    ref = evs[0].eval(exprs[-1])
    for ev in evs:
        got = ev.eval(exprs[-1])
        if got != ref:
            print(f"MISMATCH [{ev.name}]: {got} != {ref}")

    for ev in evs:
        row = f"{ev.name:<26}"
        for m, expr in zip(SIZES, exprs):
            best = min(_time_one(ev, expr) for _ in range(REPS))
            row += f"{best / (m * leaves_per_factor) * 1e9:>12.0f}"
        print(row)
    print()


def _time_one(ev, expr):
    t0 = time.perf_counter()
    ev.eval(expr)
    return time.perf_counter() - t0


def main():
    print("== Python: adversarial chains (structured inputs, no parens) ==\n")
    run_shape("powchain: b^e * b^e / ... (mixed precedence)", pow_chain, 2)
    run_shape("sumchain: 1 + 2 - 3 + ... (single precedence — control)", sum_chain, 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
