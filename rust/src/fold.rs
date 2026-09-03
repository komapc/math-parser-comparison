//! multipass-reverse, fused ("fold"): the same bottom-up order as reverse.rs —
//! deepest parens, then ^/unary, then `* /`, then `+ -` — with the two binary
//! levels contracted in the SAME left-to-right sweep that materialises the
//! items. After a ^/unary segment folds on a barrier, only two
//! left-associative levels remain, and those need two accumulators (term, sum)
//! with a pending operator each, not a list. A '(' pushes the accumulators on
//! a frame stack, so there is no recursion, no paren-matching prepass and no
//! per-level re-scan. Port of cpp/src/multipass_reverse_fold.cpp, including
//! the two-mode (operand / operator) scan, the register fast path for a
//! signed leaf and streaming tokens with one token of lookahead.
//!
//! Mechanically it is an operator-precedence shift-reduce parser with the
//! four levels hard-coded; see the README.
//!
//! Stack discipline: like the C++ version, both stacks are pre-sized raw
//! buffers indexed by local counters (the one `unsafe` in the crate). Each
//! token pushes at most one item / one frame and there is at most one token
//! per source byte, so `src.len() + 1` bounds both. The `Vec` push/pop form
//! costs ~3 % of instructions on the tree variant and ~7 % on direct-reverse,
//! measured by `perf stat`; see README.md.

use crate::builder::Builder;
use crate::lexer::{Kind, Lexer, Token};
use crate::Error;

#[derive(Clone, Copy)]
enum Seg<V: Copy> {
    Un(Kind),     // pending prefix sign
    PowBase(V),   // operand that turned out to be the base of a ^
}

#[derive(Clone, Copy)]
struct Frame<V: Copy> {
    term: V,
    sum: V,
    seg_base: u32,
    pend_mul: Kind, // End = nothing pending
    pend_add: Kind,
}

/// Working memory, owned by the evaluator and reused across evals (warm).
pub struct FoldState<V: Copy> {
    seg: Vec<Seg<V>>,
    frames: Vec<Frame<V>>,
}

impl<V: Copy> Default for FoldState<V> {
    fn default() -> Self { FoldState { seg: Vec::new(), frames: Vec::new() } }
}

pub fn fold_parse<B: Builder>(src: &str, b: &mut B, st: &mut FoldState<B::V>) -> Result<B::V, Error>
where B::V: Copy + Default {
    // Each token pushes at most one item / one frame, and there is at most
    // one token per source byte, so the source length bounds both stacks.
    let bound = src.len() + 1;
    if st.seg.len() < bound {
        st.seg.resize(bound, Seg::Un(Kind::End));
        st.frames.resize(bound, Frame { term: Default::default(), sum: Default::default(), seg_base: 0, pend_mul: Kind::End, pend_add: Kind::End });
    }
    let seg: *mut Seg<B::V> = st.seg.as_mut_ptr();
    let frames: *mut Frame<B::V> = st.frames.as_mut_ptr();
    let mut seg_top: u32 = 0;
    let mut seg_base: u32 = 0;
    let mut n_frames: u32 = 0;

    // The live frame is scalars; a Frame is only materialised on '('.
    let mut term: B::V = Default::default();
    let mut sum: B::V = Default::default();
    let mut val: B::V;
    let mut pend_mul = Kind::End;
    let mut pend_add = Kind::End;

    let mut lx = Lexer::new(src);
    let mut cur: Token = lx.next()?;

    // Fold the segment ending in `c` right-to-left (right-assoc ^; ^ binds
    // tighter than a prefix sign). The state machine guarantees the buffer
    // above seg_base is a well-formed (Un | PowBase)* prefix.
    // SAFETY: seg_top <= bound always holds (one push per token at most),
    // and seg_base <= seg_top by construction.
    #[inline]
    unsafe fn fold_segment<B: Builder>(b: &mut B, seg: *mut Seg<B::V>, seg_top: &mut u32, seg_base: u32, c: B::V) -> B::V
    where B::V: Copy {
        if *seg_top == seg_base { return c; } // lone operand: no memory touched
        let mut acc = c;
        let mut i = *seg_top;
        loop {
            i -= 1;
            acc = match *seg.add(i as usize) {
                Seg::Un(k) => b.unary(k, acc),
                Seg::PowBase(v) => b.pow(v, acc),
            };
            if i <= seg_base { break; }
        }
        *seg_top = seg_base;
        acc
    }

    loop {
        // -- operand mode: number, variable, '(' or a prefix sign
        loop {
            let t = cur;
            cur = lx.next()?;
            // if-chain ordered by corpus frequency, as in the C++ version
            // (a `match` compiles to the same code under LLVM; kept for symmetry)
            let k = t.kind;
            if k == Kind::Num { val = b.num(t.value); break; }
            if k == Kind::LParen {
                unsafe { *frames.add(n_frames as usize) = Frame { term, sum, seg_base, pend_mul, pend_add }; }
                n_frames += 1;
                seg_base = seg_top;
                pend_mul = Kind::End;
                pend_add = Kind::End;
                continue;
            }
            if k == Kind::Minus || k == Kind::Plus {
                // Fast path: a sign on a plain leaf that is not the base
                // of a ^ ("-16", "-a") folds straight into the register.
                if cur.kind == Kind::Num || cur.kind == Kind::Ident {
                    let leaf = cur;
                    cur = lx.next()?;
                    let lv = if leaf.kind == Kind::Num { b.num(leaf.value) } else { b.var(leaf.value as usize) };
                    if cur.kind != Kind::Caret { val = b.unary(k, lv); break; }
                    unsafe { *seg.add(seg_top as usize) = Seg::Un(k); }
                    seg_top += 1;
                    val = lv;
                    break;
                }
                unsafe { *seg.add(seg_top as usize) = Seg::Un(k); }
                seg_top += 1;
                continue;
            }
            if k == Kind::Ident { val = b.var(t.value as usize); break; }
            return Err(Error::at("expected operand", t.pos));
        }
        // -- operator mode: binary op, ')' or end; ')' stays in this mode
        loop {
            let t = cur;
            cur = lx.next()?;
            let k = t.kind;
            if k == Kind::Plus || k == Kind::Minus {
                // + - barrier: closes the segment, the term and the sum
                let v = unsafe { fold_segment(b, seg, &mut seg_top, seg_base, val) };
                term = if pend_mul == Kind::End { v } else { b.mul_div(pend_mul, term, v) };
                sum = if pend_add == Kind::End { term } else { b.add_sub(pend_add, sum, term) };
                pend_add = k;
                pend_mul = Kind::End;
                break;
            }
            if k == Kind::RParen {
                if n_frames == 0 { return Err(Error::at("mismatched parenthesis", t.pos)); }
                n_frames -= 1;
                let f = unsafe { *frames.add(n_frames as usize) };
                let v = unsafe { fold_segment(b, seg, &mut seg_top, seg_base, val) };
                let tv = if pend_mul == Kind::End { v } else { b.mul_div(pend_mul, term, v) };
                val = if pend_add == Kind::End { tv } else { b.add_sub(pend_add, sum, tv) };
                term = f.term; sum = f.sum; seg_base = f.seg_base;
                pend_mul = f.pend_mul; pend_add = f.pend_add;
                continue;
            }
            if k == Kind::Star || k == Kind::Slash {
                // * / barrier: closes the segment into the term
                let v = unsafe { fold_segment(b, seg, &mut seg_top, seg_base, val) };
                term = if pend_mul == Kind::End { v } else { b.mul_div(pend_mul, term, v) };
                pend_mul = k;
                break;
            }
            if k == Kind::Caret {
                unsafe { *seg.add(seg_top as usize) = Seg::PowBase(val); }
                seg_top += 1;
                break;
            }
            if k == Kind::End {
                if n_frames != 0 { return Err(Error("missing ')'".into())); }
                let v = unsafe { fold_segment(b, seg, &mut seg_top, seg_base, val) };
                let tv = if pend_mul == Kind::End { v } else { b.mul_div(pend_mul, term, v) };
                return Ok(if pend_add == Kind::End { tv } else { b.add_sub(pend_add, sum, tv) });
            }
            return Err(Error::at("expected operator", t.pos));
        }
    }
}
