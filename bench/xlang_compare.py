#!/usr/bin/env python3
"""Compare the cross-language outputs of the four implementations.

usage: python3 bench/xlang_compare.py INPUTS... -- OUT1 OUT2 ...

INPUTS are the files every `xlang` tool read (in the same order); each OUT is
one tool's output: one line per input line, `E` (rejected), `nan`, or the
IEEE-754 bits in hex. Every OUT must agree with the first on accept/reject and,
for values, within the relative 1e-9 every spec suite uses (libm `pow` may
differ by an ulp between languages). Exit 1 on any disagreement.

Within one language every strategy shares a lexer, so its own differential
fuzz cannot catch a number-parsing bug; Haskell's `reads` turning
`1e-10000000000000000000` into inf went unseen that way until 2026-11.
"""
import math
import struct
import sys


def value(tok):
    if tok in ("E", "nan"):
        return tok
    return struct.unpack(">d", bytes.fromhex(tok))[0]


def agree(a, b):
    if isinstance(a, str) or isinstance(b, str):
        return a == b
    if a == b:  # covers ±inf
        return True
    if math.isinf(a) or math.isinf(b):
        return False
    return abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))


def main():
    args = sys.argv[1:]
    k = args.index("--")
    inputs, outs = args[:k], args[k + 1:]
    exprs = [e for p in inputs for e in open(p).read().splitlines()]
    res = {p: open(p).read().split() for p in outs}
    bad = 0
    for p, r in res.items():
        if len(r) != len(exprs):
            print(f"{p}: {len(r)} results for {len(exprs)} inputs")
            bad += 1
    if bad:
        sys.exit(1)
    ref = outs[0]
    for p in outs[1:]:
        for i, e in enumerate(exprs):
            a, b = value(res[ref][i]), value(res[p][i])
            if not agree(a, b):
                bad += 1
                if bad <= 20:
                    print(f"MISMATCH on {e!r}: {ref}={a!r} vs {p}={b!r}")
    print(f"{len(exprs)} inputs x {len(outs)} implementations, {bad} disagreement(s)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
