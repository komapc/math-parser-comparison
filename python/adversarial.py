#!/usr/bin/env python3
"""Adversarial (structured, non-random) inputs that separate the multipass family.

Three flat chains plus one nested shape (mirrors cpp/bench/adversarial_bench.cpp):

  powchain   3^2 * 2^2 / 2^2 * 3^3 / 3^3 ...   alternating-precedence (^ vs */)
  towerchain 1^1^...^1 * 1 * 1 * ...           long ^ run, then a * run
  sumchain   1 + 2 - 3 + 4 ...                 single-precedence (control)
  nestchain  (((...(1 + 1)...) + 1)            deep parenthesis nesting

On powchain the top-down splitters degenerate: the candidate list mixes prec 2
and prec 4, so the flat-chain fold never applies and the linear right-to-left
split scan has no prec-1 early exit — every split rescans its whole range:
Theta(n^2) (multipass, multipass-arena). The sparse-table RMQ (multipass-bfs)
answers splits in O(1). towerchain attacks the OTHER linear scan, the
flat-chain *check*: every right-end * split leaves the long same-precedence ^
prefix in the left sub-range and rereads it — quadratic even with O(1) splits,
so it also catches multipass-bfs. Bottom-up multipass-reverse reduces each
level in one pass regardless: Theta(n). sumchain is the control where the
whole family stays linear.

NOTE: the shipped top-down variants include the O(n log n) bounded-scan +
bucket fix, so the quadratic behaviour above is pre-fix; nestchain is the
shape adversarial to BOTTOM-UP (its only recursion is per paren group).
Full pre/post-fix expectations: docs/multipass-reverse.md.
"""
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mathparser import all_evaluators  # noqa: E402

sys.setrecursionlimit(200_000)

SIZES = [64, 256, 1024]
REPS = 5


def pow_chain(m):
    # adjacent factor pairs are identical, so the value stays exactly 9.0
    parts = ["3 ^ 2"]
    for i in range(1, m):
        k = (i - 1) // 2
        parts.append(" * " if i % 2 else " / ")
        parts.append(f"{2 + k % 8} ^ {2 + k % 3}")
    return "".join(parts)


def tower_chain(m):
    # m//2 ^ ops in one run, then the rest * ops, all operands 1 — value 1.0
    return "1" + " ^ 1" * (m // 2) + " * 1" * (m - m // 2)


def sum_chain(m):
    parts = ["1"]
    for i in range(1, m):
        parts.append(" + " if i % 2 else " - ")
        parts.append(str(1 + i % 9))
    return "".join(parts)


def nest_chain(m):
    # m nested paren groups: (((...(1 + 1)...) + 1) — value m+1, m+1 leaves
    return "(" * m + "1" + " + 1)" * m


def run_shape(title, gen, leaves):
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
            row += f"{best / leaves(m) * 1e9:>12.0f}"
        print(row)
    print()


def _time_one(ev, expr):
    t0 = time.perf_counter()
    ev.eval(expr)
    return time.perf_counter() - t0


def main():
    print("== Python: adversarial chains (structured inputs) ==\n")
    run_shape("powchain: b^e * b^e / ... (mixed precedence)",
              pow_chain, lambda m: 2 * m)
    run_shape("towerchain: 1^1^...^1 * 1 * ... (flat-check attack)",
              tower_chain, lambda m: m + 1)
    run_shape("sumchain: 1 + 2 - 3 + ... (single precedence — control)",
              sum_chain, lambda m: m)
    run_shape("nestchain: (((...(1 + 1)...) + 1) (deep nesting — bottom-up's turn)",
              nest_chain, lambda m: m + 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
