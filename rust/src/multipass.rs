//! The top-down divide-and-conquer family ("multipass" in the original,
//! wrong-order sense): find the loosest operator of a range, split, recurse.
//! Port of python/mathparser/evaluators.py `_MP`: candidates are bucketed per
//! paren depth, a right-to-left scan finds the split with a SCAN_BUDGET, and
//! long single-level runs fold from per-precedence buckets so the two former
//! quadratic shapes (powchain / towerchain) are O(n log n). `with_sparse`
//! selects the sparse-table RMQ split (multipass-bfs).

use crate::builder::{op_of, Builder};
use crate::classics::{bin_prec, UNARY_PREC};
use crate::lexer::{Kind, Token};
use crate::Error;

#[derive(Clone, Copy)]
struct Cand {
    pos: u32,
    prec: u8,
    right_assoc: bool,
    unary: bool,
}

#[inline]
fn better(a: &Cand, b: &Cand) -> bool {
    if a.prec != b.prec {
        return a.prec < b.prec;
    }
    if a.right_assoc { a.pos < b.pos } else { a.pos > b.pos }
}

const SCAN_BUDGET: usize = 16;

/// Per-eval working state; the vectors are owned by the evaluator and reused.
#[derive(Default)]
pub struct MpState {
    cbd: Vec<Vec<Cand>>,      // candidates per depth
    nd: Vec<Vec<u32>>,        // next-class-change index per depth
    pm: Vec<u32>,             // paren match
    bks: Vec<[Vec<u32>; 2]>,  // lazily built prec-1 / prec-2 buckets per depth
    bks_built: bool,
    st: Vec<Vec<Vec<u32>>>,   // sparse table per depth (bfs only)
}

struct Mp<'t, 's> {
    t: &'t [Token],
    s: &'s mut MpState,
    with_sparse: bool,
}

pub fn mp_parse<B: Builder>(tokens: &[Token], b: &mut B, st: &mut MpState, with_sparse: bool) -> Result<B::V, Error> {
    build_candidates(tokens, st)?;
    st.bks_built = false;
    if with_sparse {
        build_sparse(st);
    }
    let mut m = Mp { t: tokens, s: st, with_sparse };
    m.range(b, 0, tokens.len() - 1, 0)
}

fn build_candidates(t: &[Token], s: &mut MpState) -> Result<(), Error> {
    for v in &mut s.cbd { v.clear(); }
    s.pm.clear();
    s.pm.resize(t.len(), 0);
    let mut depth = 0usize;
    let mut expect_operand = true;
    let mut stack: Vec<u32> = Vec::new();
    for i in 0..t.len() - 1 {
        let k = t[i].kind;
        match k {
            Kind::LParen => { stack.push(i as u32); depth += 1; expect_operand = true; }
            Kind::RParen => {
                let o = stack.pop().ok_or_else(|| Error("mismatched parenthesis".into()))?;
                s.pm[o as usize] = i as u32; s.pm[i] = o;
                depth -= 1; expect_operand = false;
            }
            Kind::Num | Kind::Ident => expect_operand = false,
            Kind::End => {}
            _ => {
                let c = if expect_operand {
                    if !matches!(k, Kind::Plus | Kind::Minus) { return Err(Error("unexpected operator".into())); }
                    Cand { pos: i as u32, prec: UNARY_PREC, right_assoc: false, unary: true }
                } else {
                    Cand { pos: i as u32, prec: bin_prec(k), right_assoc: k == Kind::Caret, unary: false }
                };
                while depth >= s.cbd.len() { s.cbd.push(Vec::new()); }
                s.cbd[depth].push(c);
                expect_operand = true;
            }
        }
    }
    // next-class-change index
    while s.nd.len() < s.cbd.len() { s.nd.push(Vec::new()); }
    for (d, vd) in s.cbd.iter().enumerate() {
        let k = vd.len();
        let nd = &mut s.nd[d];
        nd.clear();
        nd.resize(k, 0);
        for i in (0..k).rev() {
            nd[i] = if i + 1 < k && vd[i + 1].prec == vd[i].prec { nd[i + 1] } else { (i + 1) as u32 };
        }
    }
    Ok(())
}

fn build_buckets(s: &mut MpState) {
    while s.bks.len() < s.cbd.len() { s.bks.push([Vec::new(), Vec::new()]); }
    for (d, vd) in s.cbd.iter().enumerate() {
        let [b1, b2] = &mut s.bks[d];
        b1.clear(); b2.clear();
        for (i, c) in vd.iter().enumerate() {
            if c.unary { continue; }
            if c.prec == 1 { b1.push(i as u32); } else if c.prec == 2 { b2.push(i as u32); }
        }
    }
    s.bks_built = true;
}

fn build_sparse(s: &mut MpState) {
    while s.st.len() < s.cbd.len() { s.st.push(Vec::new()); }
    for (d, v) in s.cbd.iter().enumerate() {
        let n = v.len();
        let tbl = &mut s.st[d];
        tbl.clear();
        if n == 0 { continue; }
        let logn = usize::BITS as usize - n.leading_zeros() as usize;
        tbl.push((0..n as u32).collect());
        for k in 1..logn {
            let half = 1usize << (k - 1);
            let prev = &tbl[k - 1];
            let mut row = vec![0u32; n];
            for i in 0..=(n - (1usize << k)) {
                let (a, b) = (prev[i], prev[i + half]);
                row[i] = if better(&v[a as usize], &v[b as usize]) { a } else { b };
            }
            tbl.push(row);
        }
    }
}

#[inline]
fn lower_bound_pos(v: &[Cand], x: u32) -> usize {
    v.partition_point(|c| c.pos < x)
}

impl<'t, 's> Mp<'t, 's> {
    fn st_query(&self, depth: usize, lo: usize, hi: usize) -> i64 {
        if lo >= hi { return -1; }
        let v = &self.s.cbd[depth];
        let t = &self.s.st[depth];
        let k = (usize::BITS - (hi - lo).leading_zeros() - 1) as usize;
        let (a, b) = (t[k][lo], t[k][hi - (1usize << k)]);
        (if better(&v[a as usize], &v[b as usize]) { a } else { b }) as i64
    }

    /// Returns split index, -1 (no candidate: leaf/paren) or -2 (budget
    /// exhausted: fold the level from the buckets).
    fn split(&self, b: usize, e: usize, lo: usize, depth: usize) -> i64 {
        if b >= e { return -1; } // no candidates (depth may exceed the table)
        let v = &self.s.cbd[depth];
        if self.with_sparse {
            let si = self.st_query(depth, b, e);
            if si < 0 { return -1; }
            let c = v[si as usize];
            if c.unary && c.pos as usize != lo {
                for i in b..e {
                    if !v[i].unary || v[i].pos as usize == lo { return i as i64; }
                }
                return -1;
            }
            return si;
        }
        let mut best: i64 = -1;
        let stop = if e - b > SCAN_BUDGET { e - SCAN_BUDGET } else { b };
        let mut i = e;
        while i > stop {
            i -= 1;
            let c = v[i];
            if c.unary && c.pos as usize != lo { continue; }
            if best == -1 || c.prec < v[best as usize].prec {
                best = i as i64;
                if c.prec == 1 && !c.right_assoc { return best; }
            } else if c.prec == v[best as usize].prec && v[best as usize].right_assoc {
                best = i as i64;
            }
        }
        if stop == b { best } else { -2 }
    }

    fn level_fold<B: Builder>(&mut self, bl: &mut B, b: usize, e: usize, lo: usize, hi: usize, depth: usize) -> Result<B::V, Error> {
        if !self.s.bks_built { build_buckets(self.s); }
        for which in 0..2 {
            let bk = &self.s.bks[depth][which];
            let i0 = bk.partition_point(|&x| (x as usize) < b);
            let i1 = i0 + bk[i0..].partition_point(|&x| (x as usize) < e);
            if i0 < i1 {
                let idx: Vec<u32> = bk[i0..i1].to_vec();
                let v = &self.s.cbd[depth];
                let first = v[idx[0] as usize].pos as usize;
                let mut acc = self.range(bl, lo, first, depth)?;
                for j in 0..idx.len() {
                    let pos = self.s.cbd[depth][idx[j] as usize].pos as usize;
                    let nhi = if j + 1 < idx.len() { self.s.cbd[depth][idx[j + 1] as usize].pos as usize } else { hi };
                    let rhs = self.range(bl, pos + 1, nhi, depth)?;
                    acc = bl.binop(op_of(self.t[pos].kind), acc, rhs);
                }
                return Ok(acc);
            }
        }
        let c = self.s.cbd[depth][b];
        let pos = c.pos as usize;
        if c.unary {
            let operand = self.range(bl, pos + 1, hi, depth)?;
            return Ok(if self.t[pos].kind == Kind::Minus { bl.neg(operand) } else { bl.pos(operand) });
        }
        let l = self.range(bl, lo, pos, depth)?;
        let r = self.range(bl, pos + 1, hi, depth)?;
        Ok(bl.binop(op_of(self.t[pos].kind), l, r))
    }

    fn range<B: Builder>(&mut self, bl: &mut B, lo: usize, hi: usize, depth: usize) -> Result<B::V, Error> {
        if lo >= hi { return Err(Error("empty sub-expression".into())); }
        let (b, e) = if depth < self.s.cbd.len() {
            let v = &self.s.cbd[depth];
            let b = lower_bound_pos(v, lo as u32);
            (b, b + lower_bound_pos(&v[b..], hi as u32))
        } else { (0, 0) };

        // flat chain: all candidates share one precedence — fold iteratively
        if b < e {
            let c0 = self.s.cbd[depth][b];
            if !c0.unary && self.s.nd[depth][b] as usize >= e {
                if !c0.right_assoc {
                    let mut acc = self.range(bl, lo, c0.pos as usize, depth)?;
                    for i in b..e {
                        let pos = self.s.cbd[depth][i].pos as usize;
                        let nhi = if i + 1 < e { self.s.cbd[depth][i + 1].pos as usize } else { hi };
                        let rhs = self.range(bl, pos + 1, nhi, depth)?;
                        acc = bl.binop(op_of(self.t[pos].kind), acc, rhs);
                    }
                    return Ok(acc);
                }
                // right fold (^)
                let mut parts: Vec<B::V> = Vec::with_capacity(e - b + 1);
                let mut prev = lo;
                for i in b..e {
                    let pos = self.s.cbd[depth][i].pos as usize;
                    parts.push(self.range(bl, prev, pos, depth)?);
                    prev = pos + 1;
                }
                parts.push(self.range(bl, prev, hi, depth)?);
                let mut acc = parts.pop().unwrap();
                while let Some(p) = parts.pop() {
                    let pos = self.s.cbd[depth][b + parts.len()].pos as usize;
                    acc = bl.binop(op_of(self.t[pos].kind), p, acc);
                }
                return Ok(acc);
            }
        }

        let s = self.split(b, e, lo, depth);
        if s == -2 { return self.level_fold(bl, b, e, lo, hi, depth); }
        if s >= 0 {
            let c = self.s.cbd[depth][s as usize];
            let pos = c.pos as usize;
            if c.unary {
                let operand = self.range(bl, pos + 1, hi, depth)?;
                return Ok(if self.t[pos].kind == Kind::Minus { bl.neg(operand) } else { bl.pos(operand) });
            }
            let l = self.range(bl, lo, pos, depth)?;
            let r = self.range(bl, pos + 1, hi, depth)?;
            return Ok(bl.binop(op_of(self.t[pos].kind), l, r));
        }

        // paren group or leaf
        if self.t[lo].kind == Kind::LParen && self.s.pm[lo] as usize == hi - 1 {
            return self.range(bl, lo + 1, hi - 1, depth + 1);
        }
        if hi - lo == 1 {
            let tok = self.t[lo];
            if tok.kind == Kind::Num { return Ok(bl.num(tok.value)); }
            if tok.kind == Kind::Ident { return Ok(bl.var(tok.value as usize)); }
        }
        Err(Error::at("syntax error", self.t[lo].pos))
    }
}
