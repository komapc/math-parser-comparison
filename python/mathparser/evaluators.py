"""The eleven strategies, idiomatic Python.

The axis that actually distinguishes them is *representation* × *parse order*:

  representation         builder        used by
  ------------------     -----------    -------------------------------------
  pointer AST (tuples)   TupleBuilder   ast-*, multipass
  arena (flat list)      ArenaBuilder   ast-arena, multipass-arena/-bfs
  none (inline float)    DirectBuilder  direct-*

  parse order            driver         used by
  ------------------     -----------    -------------------------------------
  recursive descent      rd_parse       ast-recursive-descent, ast-arena, direct-rd
  shunting-yard          sy_parse       ast-shunting-yard, direct-shunting-yard
  Pratt                  pratt_parse    ast-pratt
  divide & conquer       _MP.parse      multipass, multipass-arena, -bfs, direct-mp
  (bytecode is its own compile-then-run two-phase)

A builder exposes num/var/neg/pos/binop returning its representation, plus
result() to collapse the root to a float given the variable environment.
"""
import math
from bisect import bisect_left

from .lexer import (
    NUM, IDENT, PLUS, MINUS, STAR, SLASH, CARET, LPAREN, RPAREN, END, tokenize,
)

BIN_CHAR = {PLUS: "+", MINUS: "-", STAR: "*", SLASH: "/", CARET: "^"}
UNARY_PREC = 3


def bin_prec(kind: int) -> int:
    if kind in (PLUS, MINUS):
        return 1
    if kind in (STAR, SLASH):
        return 2
    if kind == CARET:
        return 4
    return -1


# ---- IEEE-faithful arithmetic (match C++ double: no exceptions, nan/inf) -----
def _div(a: float, b: float) -> float:
    try:
        return a / b
    except ZeroDivisionError:
        return float("nan") if a == 0.0 else math.copysign(math.inf, a)


def _pow(a: float, b: float) -> float:
    try:
        return math.pow(a, b)
    except ValueError:
        return float("nan")        # e.g. negative base, fractional exponent
    except OverflowError:
        return math.inf


def apply_bin(op: str, l: float, r: float) -> float:
    if op == "+":
        return l + r
    if op == "-":
        return l - r
    if op == "*":
        return l * r
    if op == "/":
        return _div(l, r)
    return _pow(l, r)  # "^"


# ---- representations (builders) ---------------------------------------------
def eval_ast(node, vars):
    k = node[0]
    if k == "num":
        return node[1]
    if k == "var":
        return vars[node[1]] if vars is not None else 0.0
    if k == "neg":
        return -eval_ast(node[1], vars)
    if k == "pos":
        return eval_ast(node[1], vars)
    return apply_bin(k, eval_ast(node[1], vars), eval_ast(node[2], vars))


class TupleBuilder:
    """Pointer-AST analog: nested immutable tuples."""

    def __init__(self, vars):
        self.vars = vars

    def num(self, v):       return ("num", v)
    def var(self, i):       return ("var", i)
    def neg(self, a):       return ("neg", a)
    def pos(self, a):       return ("pos", a)
    def binop(self, op, l, r): return (op, l, r)
    def result(self, root): return eval_ast(root, self.vars)


class ArenaBuilder:
    """Arena analog: one flat list; children referenced by integer index."""

    def __init__(self, vars):
        self.vars = vars
        self.nodes = []  # each: (kind, a, b, value)

    def _emit(self, node):
        self.nodes.append(node)
        return len(self.nodes) - 1

    def num(self, v):       return self._emit(("num", -1, -1, v))
    def var(self, i):       return self._emit(("var", i, -1, 0.0))
    def neg(self, a):       return self._emit(("neg", a, -1, 0.0))
    def pos(self, a):       return self._emit(("pos", a, -1, 0.0))
    def binop(self, op, l, r): return self._emit((op, l, r, 0.0))

    def _walk(self, i):
        kind, a, b, value = self.nodes[i]
        if kind == "num":
            return value
        if kind == "var":
            return self.vars[a] if self.vars is not None else 0.0
        if kind == "neg":
            return -self._walk(a)
        if kind == "pos":
            return self._walk(a)
        return apply_bin(kind, self._walk(a), self._walk(b))

    def result(self, root):
        return self._walk(root)


class DirectBuilder:
    """No tree: every node collapses to a float as it is built."""

    def __init__(self, vars):
        self.vars = vars

    def num(self, v):       return v
    def var(self, i):       return self.vars[i] if self.vars is not None else 0.0
    def neg(self, a):       return -a
    def pos(self, a):       return a
    def binop(self, op, l, r): return apply_bin(op, l, r)
    def result(self, root): return root


# ---- driver: recursive descent ----------------------------------------------
def rd_parse(tokens, B):
    i = 0

    def expr():
        nonlocal i
        left = term()
        while tokens[i].kind in (PLUS, MINUS):
            op = BIN_CHAR[tokens[i].kind]; i += 1
            left = B.binop(op, left, term())
        return left

    def term():
        nonlocal i
        left = unary()
        while tokens[i].kind in (STAR, SLASH):
            op = BIN_CHAR[tokens[i].kind]; i += 1
            left = B.binop(op, left, unary())
        return left

    def unary():
        nonlocal i
        k = tokens[i].kind
        if k in (PLUS, MINUS):
            i += 1
            c = unary()
            return B.neg(c) if k == MINUS else B.pos(c)
        return power()

    def power():
        nonlocal i
        base = primary()
        if tokens[i].kind == CARET:
            i += 1
            return B.binop("^", base, unary())
        return base

    def primary():
        nonlocal i
        tok = tokens[i]
        if tok.kind == NUM:
            i += 1; return B.num(tok.value)
        if tok.kind == IDENT:
            i += 1; return B.var(int(tok.value))
        if tok.kind == LPAREN:
            i += 1
            e = expr()
            if tokens[i].kind != RPAREN:
                raise ValueError(f"expected ')' at position {tokens[i].pos}")
            i += 1; return e
        raise ValueError(f"expected number or '(' at position {tok.pos}")

    root = expr()
    if tokens[i].kind != END:
        raise ValueError(f"unexpected token at position {tokens[i].pos}")
    return root


# ---- driver: Pratt / precedence climbing ------------------------------------
def pratt_parse(tokens, B):
    i = 0

    def lbp(kind):
        p = bin_prec(kind)
        return p if p > 0 else 0

    def parse(rbp):
        nonlocal i
        tok = tokens[i]; i += 1
        left = nud(tok)
        while True:
            k = tokens[i].kind
            lb = lbp(k)
            if lb <= rbp:
                break
            i += 1
            ra = (k == CARET)
            right = parse(lb - 1 if ra else lb)
            left = B.binop(BIN_CHAR[k], left, right)
        return left

    def nud(tok):
        nonlocal i
        k = tok.kind
        if k == NUM:
            return B.num(tok.value)
        if k == IDENT:
            return B.var(int(tok.value))
        if k in (PLUS, MINUS):
            operand = parse(UNARY_PREC)  # rbp 3 < ^ lbp 4, so prefix grabs a power
            return B.neg(operand) if k == MINUS else B.pos(operand)
        if k == LPAREN:
            e = parse(0)
            if tokens[i].kind != RPAREN:
                raise ValueError(f"expected ')' at position {tokens[i].pos}")
            i += 1
            return e
        raise ValueError(f"unexpected token at position {tok.pos}")

    root = parse(0)
    if tokens[i].kind != END:
        raise ValueError(f"unexpected token at position {tokens[i].pos}")
    return root


# ---- driver: shunting-yard --------------------------------------------------
# Operator stack entries: (kind, prec, right_assoc, unary, lparen).
def sy_parse(tokens, B):
    out = []
    ops = []

    def emit(op):
        kind, _prec, _ra, unary, lparen = op
        if lparen:
            raise ValueError("mismatched parenthesis")
        if unary:
            x = out.pop()
            out.append(B.neg(x) if kind == MINUS else B.pos(x))
        else:
            r = out.pop(); l = out.pop()
            out.append(B.binop(BIN_CHAR[kind], l, r))

    expect_operand = True
    for tok in tokens:
        k = tok.kind
        if k == NUM:
            if not expect_operand:
                raise ValueError("unexpected number")
            out.append(B.num(tok.value)); expect_operand = False
        elif k == IDENT:
            if not expect_operand:
                raise ValueError("unexpected variable")
            out.append(B.var(int(tok.value))); expect_operand = False
        elif k == LPAREN:
            ops.append((LPAREN, 0, False, False, True)); expect_operand = True
        elif k == RPAREN:
            while ops and not ops[-1][4]:
                emit(ops.pop())
            if not ops:
                raise ValueError("mismatched parenthesis")
            ops.pop(); expect_operand = False
        elif k in (PLUS, MINUS, STAR, SLASH, CARET):
            if expect_operand:
                if k not in (PLUS, MINUS):
                    raise ValueError("unexpected operator")
                ops.append((k, UNARY_PREC, True, True, False))
            else:
                prec = bin_prec(k); ra = (k == CARET)
                while (ops and not ops[-1][4] and
                       (ops[-1][1] > prec or (ops[-1][1] == prec and not ra))):
                    emit(ops.pop())
                ops.append((k, prec, ra, False, False))
                expect_operand = True
        elif k == END:
            if expect_operand:
                raise ValueError("unexpected end of input")
    while ops:
        emit(ops.pop())
    if not out:
        raise ValueError("invalid expression")
    return out[-1]


# ---- driver: divide & conquer (multipass family) ----------------------------
# Candidate: (pos, prec, right_assoc, unary).
def _build_candidates(tokens):
    n = len(tokens)
    cbd = []            # candidates bucketed by paren-nesting depth
    paren_match = [0] * n
    depth = 0
    expect_operand = True
    stack = []
    for i in range(n - 1):  # exclude END sentinel
        k = tokens[i].kind
        if k == LPAREN:
            stack.append(i); depth += 1; expect_operand = True
        elif k == RPAREN:
            if stack:
                o = stack.pop(); paren_match[o] = i; paren_match[i] = o
            depth -= 1; expect_operand = False
        elif k in (NUM, IDENT):
            expect_operand = False
        elif k in (PLUS, MINUS, STAR, SLASH, CARET):
            if expect_operand:
                if k not in (PLUS, MINUS):
                    raise ValueError("unexpected operator")
                cand = (i, UNARY_PREC, False, True)
            else:
                cand = (i, bin_prec(k), k == CARET, False)
            while depth >= len(cbd):
                cbd.append([])
            cbd[depth].append(cand)
            expect_operand = True
    return cbd, paren_match


def _better(a, b):
    # True if a should be the root: lower prec wins; on a tie, right-assoc
    # prefers the leftmost, left-assoc the rightmost.
    if a[1] != b[1]:
        return a[1] < b[1]
    return (a[0] < b[0]) if a[2] else (a[0] > b[0])


# Linear scans inside _MP give up after this many candidates and consult the
# per-precedence buckets instead (see _MP._buckets) — caps the two former
# Theta(n^2) worst cases (powchain / towerchain in adversarial.py) at
# O(n log n) while leaving short ranges on the unchanged fast path.
SCAN_BUDGET = 16


class _MP:
    def __init__(self, tokens, with_sparse):
        self.t = tokens
        self.cbd, self.pm = _build_candidates(tokens)
        self.st = self._build_sparse() if with_sparse else None
        self.bks = None  # per-depth precedence buckets, built on first need
        # nd[d][i]: first index after i whose candidate has a different
        # precedence class (unary prec 3 is its own class). One O(n) pass
        # makes every "is [b, e) flat?" question a single O(1) lookup:
        # flat iff v[b] is binary and nd[d][b] >= e.
        self.nd = []
        for vd in self.cbd:
            k = len(vd)
            nd = [0] * k
            for i in range(k - 1, -1, -1):
                if i + 1 < k and vd[i + 1][1] == vd[i][1]:
                    nd[i] = nd[i + 1]
                else:
                    nd[i] = i + 1
            self.nd.append(nd)

    # Per depth, the candidate *indices* of each precedence class, sorted —
    # (prec-1, prec-2, caret, unary). Lets a long range answer "rightmost
    # prec-1/-2 in [b, e)" and "is [b, e) single-class?" by bisect instead of
    # an O(k) rescan. Built lazily: random expressions rarely need it.
    def _buckets(self):
        if self.bks is None:
            self.bks = []
            for vd in self.cbd:
                b1, b2, bc, bu = [], [], [], []
                for i, (_, prec, _, unary) in enumerate(vd):
                    (bu if unary else
                     b1 if prec == 1 else
                     b2 if prec == 2 else bc).append(i)
                self.bks.append((b1, b2, bc, bu))
        return self.bks

    def _level_fold(self, B, v, b, e, lo, hi, depth):
        # Bucket fallback, folding a whole precedence level at once: the
        # lowest class present in [b, e) is prec-1, else prec-2 (both
        # left-assoc, so an iterative left fold over *all* its operators
        # builds the same tree as repeated rightmost splits — one bucket
        # slice per level instead of per split). Otherwise the range is
        # ^/unary only and its first candidate is the root — a leading unary
        # (lowest precedence present) or the leftmost right-assoc ^. A unary
        # not at the range start cannot be first: the token before it is a
        # same-depth operator, itself the earlier candidate.
        b1, b2, _, _ = self._buckets()[depth]
        for bk in (b1, b2):
            i0 = bisect_left(bk, b)
            i1 = bisect_left(bk, e, lo=i0)
            if i0 < i1:
                acc = self._range(B, lo, v[bk[i0]][0], depth)
                for j in range(i0, i1):
                    pos = v[bk[j]][0]
                    nhi = v[bk[j + 1]][0] if j + 1 < i1 else hi
                    rhs = self._range(B, pos + 1, nhi, depth)
                    acc = B.binop(BIN_CHAR[self.t[pos].kind], acc, rhs)
                return acc
        pos = v[b][0]
        if v[b][3]:  # leading unary
            operand = self._range(B, pos + 1, hi, depth)
            return B.neg(operand) if self.t[pos].kind == MINUS else B.pos(operand)
        l = self._range(B, lo, pos, depth)
        r = self._range(B, pos + 1, hi, depth)
        return B.binop(BIN_CHAR[self.t[pos].kind], l, r)

    # sparse table per depth: st[d][k][i] = argmin (_better) over [i, i+2^k)
    def _build_sparse(self):
        tables = []
        for v in self.cbd:
            n = len(v)
            if n == 0:
                tables.append([]); continue
            logn = n.bit_length()
            t = [[0] * n for _ in range(logn)]
            t[0] = list(range(n))
            for k in range(1, logn):
                half = 1 << (k - 1)
                row, prev = t[k], t[k - 1]
                for i in range(n - (1 << k) + 1):
                    a, b = prev[i], prev[i + half]
                    row[i] = a if _better(v[a], v[b]) else b
            tables.append(t)
        return tables

    def _st_query(self, depth, lo, hi):
        if lo >= hi:
            return -1
        v = self.cbd[depth]; t = self.st[depth]
        k = (hi - lo).bit_length() - 1
        a, b = t[k][lo], t[k][hi - (1 << k)]
        return a if _better(v[a], v[b]) else b

    def _cand_range(self, lo, hi, depth):
        v = self.cbd[depth] if 0 <= depth < len(self.cbd) else []
        b = bisect_left(v, lo, key=lambda c: c[0])
        e = bisect_left(v, hi, key=lambda c: c[0], lo=b)
        return v, b, e

    def _split(self, v, b, e, lo, depth):
        if self.st is not None:
            si = self._st_query(depth, b, e)
            if si < 0:
                return -1
            c = v[si]
            if c[3] and c[0] != lo:  # non-leading unary can't be root
                for i in range(b, e):
                    if not v[i][3] or v[i][0] == lo:
                        return i
                return -1
            return si
        # linear right-to-left scan: free left-assoc tie-break + early exit.
        # Bounded: past SCAN_BUDGET candidates without a prec-1 hit, signal
        # the caller to fold the whole level from the buckets instead (-2) —
        # one bucket slice per level, never an O(k) rescan per split.
        best = -1
        stop = e - SCAN_BUDGET if e - b > SCAN_BUDGET else b
        i = e
        while i > stop:
            i -= 1
            pos, prec, ra, unary = v[i]
            if unary and pos != lo:
                continue
            if best == -1 or prec < v[best][1]:
                best = i
                if prec == 1 and not ra:
                    return best
            elif prec == v[best][1] and v[best][2]:
                best = i
        if stop == b:
            return best  # scanned everything: exact answer
        return -2

    def parse(self, B):
        return self._range(B, 0, len(self.t) - 1, 0)

    def _range(self, B, lo, hi, depth):
        if lo >= hi:
            raise ValueError("empty sub-expression")
        v, b, e = self._cand_range(lo, hi, depth)

        # flat chain: all candidates share one precedence — fold iteratively.
        # O(1) check via the precomputed next-class-change index.
        if b < e:
            chain_prec, chain_ra = v[b][1], v[b][2]
            if not v[b][3] and self.nd[depth][b] >= e:
                if not chain_ra:  # left fold
                    acc = self._range(B, lo, v[b][0], depth)
                    for i in range(b, e):
                        pos = v[i][0]
                        nhi = v[i + 1][0] if i + 1 < e else hi
                        rhs = self._range(B, pos + 1, nhi, depth)
                        acc = B.binop(BIN_CHAR[self.t[pos].kind], acc, rhs)
                    return acc
                # right fold (^)
                parts = []
                prev = lo
                for i in range(b, e):
                    parts.append(self._range(B, prev, v[i][0], depth))
                    prev = v[i][0] + 1
                parts.append(self._range(B, prev, hi, depth))
                acc = parts[-1]
                for idx in range(len(parts) - 2, -1, -1):
                    pos = v[b + idx][0]
                    acc = B.binop(BIN_CHAR[self.t[pos].kind], parts[idx], acc)
                return acc

        # general split
        s = self._split(v, b, e, lo, depth)
        if s == -2:  # budget exhausted: fold the whole level from the buckets
            return self._level_fold(B, v, b, e, lo, hi, depth)
        if s != -1:
            pos = v[s][0]
            if v[s][3]:  # unary
                operand = self._range(B, pos + 1, hi, depth)
                return B.neg(operand) if self.t[pos].kind == MINUS else B.pos(operand)
            l = self._range(B, lo, pos, depth)
            r = self._range(B, pos + 1, hi, depth)
            return B.binop(BIN_CHAR[self.t[pos].kind], l, r)

        # paren group or leaf
        if self.t[lo].kind == LPAREN and self.pm[lo] == hi - 1:
            return self._range(B, lo + 1, hi - 1, depth + 1)
        if hi - lo == 1:
            tok = self.t[lo]
            if tok.kind == NUM:
                return B.num(tok.value)
            if tok.kind == IDENT:
                return B.var(int(tok.value))
        raise ValueError(f"syntax error at position {self.t[lo].pos}")


# ---- driver: REVERSE multipass (bottom-up, innermost/highest first) ---------
# The dual of _MP: instead of finding the lowest-precedence operator (the root)
# and splitting top-down, this reduces the *tightest-binding* things first —
# deepest parens, then ^, then */, then +- — agglomerating outward until one
# node is left. This is "multipass" in its original sense: one reduction pass
# per precedence level. Same tree, opposite construction order.
def _paren_match(tokens):
    n = len(tokens)
    pm = [0] * n
    stack = []
    for i in range(n - 1):
        k = tokens[i].kind
        if k == LPAREN:
            stack.append(i)
        elif k == RPAREN and stack:
            o = stack.pop(); pm[o] = i; pm[i] = o
    return pm


# Reduce one ^/unary segment — ('un'* 'opd' ('^' 'un'* 'opd')*) — to a single
# operand by folding RIGHT-TO-LEFT: right-assoc ^ and the ^-binds-tighter-than-
# unary corner (-a^a = -(a^a), a^-b = a^(-b)) fall out naturally, no recursion.
def _reduce_seg(seg, B):
    if not seg or seg[-1][0] != "opd":
        raise ValueError("malformed segment")
    acc = seg[-1][1]
    i = len(seg) - 2
    while i >= 0:
        kind, val = seg[i]
        if kind == "un":
            acc = B.neg(acc) if val == MINUS else B.pos(acc)
        elif kind == "op" and val == CARET and i > 0 and seg[i - 1][0] == "opd":
            i -= 1
            acc = B.binop("^", seg[i][1], acc)
        else:
            raise ValueError("malformed segment")
        i -= 1
    return acc


# Left-to-right left-associative contraction of one binary level.
def _reduce_bin_level(flat, B, ops):
    out = [flat[0]]
    i = 1
    while i < len(flat):
        op, rhs = flat[i], flat[i + 1]; i += 2
        if op[1] in ops:
            left = out.pop()
            out.append(("opd", B.binop(BIN_CHAR[op[1]], left[1], rhs[1])))
        else:
            out.append(op); out.append(rhs)
    return out


def reverse_mp_parse(tokens, B):
    pm = _paren_match(tokens)

    def close_seg(seg):
        # fast path: most segments are a lone operand
        if len(seg) == 1 and seg[0][0] == "opd":
            return seg[0]
        return ("opd", _reduce_seg(seg, B))

    def reduce_range(lo, hi):
        if lo >= hi:
            raise ValueError("empty sub-expression")
        # 1. materialise items, recursing into parens (innermost first);
        #    a */+- barrier closes the current ^/unary segment on the fly
        flat = []
        seg = []
        i = lo
        expect = True
        while i < hi:
            tok = tokens[i]; k = tok.kind
            if k == LPAREN:
                j = pm[i]
                seg.append(("opd", reduce_range(i + 1, j))); i = j + 1; expect = False
            elif k == NUM:
                seg.append(("opd", B.num(tok.value))); i += 1; expect = False
            elif k == IDENT:
                seg.append(("opd", B.var(int(tok.value)))); i += 1; expect = False
            elif k in (PLUS, MINUS, STAR, SLASH, CARET):
                if expect:
                    if k not in (PLUS, MINUS):
                        raise ValueError("unexpected operator")
                    seg.append(("un", k)); i += 1
                elif k == CARET:
                    seg.append(("op", k)); i += 1; expect = True
                else:  # barrier: close the segment
                    flat.append(close_seg(seg)); flat.append(("op", k))
                    seg = []; i += 1; expect = True
            else:
                raise ValueError("syntax error")
        if expect:
            raise ValueError("unexpected end of sub-expression")
        flat.append(close_seg(seg))

        # 2. contract */ then +- (skipped when the range was one segment)
        if len(flat) > 1:
            flat = _reduce_bin_level(flat, B, {STAR, SLASH})
            flat = _reduce_bin_level(flat, B, {PLUS, MINUS})
        if len(flat) != 1 or flat[0][0] != "opd":
            raise ValueError("reduction failed")
        return flat[0][1]

    return reduce_range(0, len(tokens) - 1)


# ---- bytecode VM (compile shunting-yard -> opcode list, then run) -----------
def compile_bytecode(tokens):
    """Shunting-yard -> reusable opcode list: ('push',v) | ('load',i) | ('neg',) | ('bin',op)."""
    code = []
    ops = []

    def emit(op):
        kind, _p, _ra, unary, lparen = op
        if lparen:
            raise ValueError("mismatched parenthesis")
        if unary:
            if kind == MINUS:
                code.append(("neg",))
            # unary plus: identity, no opcode (matches C++ bytecode)
        else:
            code.append(("bin", BIN_CHAR[kind]))

    expect_operand = True
    for tok in tokens:
        k = tok.kind
        if k == NUM:
            if not expect_operand:
                raise ValueError("unexpected number")
            code.append(("push", tok.value)); expect_operand = False
        elif k == IDENT:
            if not expect_operand:
                raise ValueError("unexpected variable")
            code.append(("load", int(tok.value))); expect_operand = False
        elif k == LPAREN:
            ops.append((LPAREN, 0, False, False, True)); expect_operand = True
        elif k == RPAREN:
            while ops and not ops[-1][4]:
                emit(ops.pop())
            if not ops:
                raise ValueError("mismatched parenthesis")
            ops.pop(); expect_operand = False
        elif k in (PLUS, MINUS, STAR, SLASH, CARET):
            if expect_operand:
                if k not in (PLUS, MINUS):
                    raise ValueError("unexpected operator")
                ops.append((k, UNARY_PREC, True, True, False))
            else:
                prec = bin_prec(k); ra = (k == CARET)
                while (ops and not ops[-1][4] and
                       (ops[-1][1] > prec or (ops[-1][1] == prec and not ra))):
                    emit(ops.pop())
                ops.append((k, prec, ra, False, False))
                expect_operand = True
        elif k == END:
            if expect_operand:
                raise ValueError("unexpected end of input")
    while ops:
        emit(ops.pop())
    if not code:
        raise ValueError("invalid expression")
    return code


def run_bytecode(code, vars):
    """Execute a compiled opcode list against a variable environment."""
    st = []
    for ins in code:
        t = ins[0]
        if t == "push":
            st.append(ins[1])
        elif t == "load":
            st.append(vars[ins[1]] if vars is not None else 0.0)
        elif t == "neg":
            st[-1] = -st[-1]
        else:  # bin
            r = st.pop()
            st[-1] = apply_bin(ins[1], st[-1], r)
    return st[-1]


def _bytecode_eval(tokens, vars):
    return run_bytecode(compile_bytecode(tokens), vars)


# ---- registry ---------------------------------------------------------------
class Evaluator:
    def __init__(self, name, fn):
        self.name = name
        self._fn = fn

    def eval(self, src, vars=None):
        return self._fn(tokenize(src), vars)


def _ast_rd(t, v):          B = TupleBuilder(v); return B.result(rd_parse(t, B))
def _ast_sy(t, v):          B = TupleBuilder(v); return B.result(sy_parse(t, B))
def _ast_pratt(t, v):       B = TupleBuilder(v); return B.result(pratt_parse(t, B))
def _ast_arena(t, v):       B = ArenaBuilder(v); return B.result(rd_parse(t, B))
def _multipass(t, v):       B = TupleBuilder(v); return B.result(_MP(t, False).parse(B))
def _multipass_arena(t, v): B = ArenaBuilder(v); return B.result(_MP(t, False).parse(B))
def _multipass_bfs(t, v):   B = ArenaBuilder(v); return B.result(_MP(t, True).parse(B))
def _multipass_reverse(t, v): B = ArenaBuilder(v); return B.result(reverse_mp_parse(t, B))
def _direct_mp(t, v):       B = DirectBuilder(v); return B.result(_MP(t, False).parse(B))
def _direct_rd(t, v):       B = DirectBuilder(v); return B.result(rd_parse(t, B))
def _direct_sy(t, v):       B = DirectBuilder(v); return B.result(sy_parse(t, B))
def _bytecode(t, v):        return _bytecode_eval(t, v)


def all_evaluators():
    """Same display order as cpp/src/evaluators.cpp."""
    return [
        Evaluator("ast-recursive-descent", _ast_rd),
        Evaluator("ast-shunting-yard", _ast_sy),
        Evaluator("ast-pratt", _ast_pratt),
        Evaluator("ast-arena", _ast_arena),
        Evaluator("multipass", _multipass),
        Evaluator("multipass-arena", _multipass_arena),
        Evaluator("direct-mp", _direct_mp),
        Evaluator("multipass-bfs", _multipass_bfs),
        Evaluator("multipass-reverse", _multipass_reverse),
        Evaluator("direct-recursive-descent", _direct_rd),
        Evaluator("direct-shunting-yard", _direct_sy),
        Evaluator("bytecode-vm", _bytecode),
    ]
