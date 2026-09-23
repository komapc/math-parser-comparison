//! multipass-reverse, buffered: the conceptual bottom-up form. Deepest parens
//! first (recursion on the pre-matched token array), then one reduction pass
//! per precedence level over an item buffer: ^/unary segments fold right to
//! left as they close, then a `* /` pass, then a `+ -` pass. Three flat
//! loops, no split-point search, linear on every input. Port of
//! cpp/src/multipass_reverse.cpp / python `reverse_mp_parse`.
//!
//! All items live in ONE shared vector with stack discipline: each
//! `reduce_range` works above its own base and truncates back on exit, so a
//! nested paren group borrows the top of the same buffer and no range ever
//! allocates.

use crate::builder::Builder;
use crate::lexer::{Kind, Token};
use crate::Error;

#[derive(Clone, Copy)]
enum Item<V: Copy> {
    Opd(V),
    Un(Kind),
    Op(Kind),
}

#[derive(Default)]
pub struct RevState<V: Copy> {
    pm: Vec<u32>,
    items: Vec<Item<V>>,
}

impl<V: Copy> RevState<V> {
    pub fn new() -> Self { RevState { pm: Vec::new(), items: Vec::new() } }
}

fn paren_match(t: &[Token], pm: &mut Vec<u32>) {
    pm.clear();
    pm.resize(t.len(), 0);
    let mut stack: Vec<u32> = Vec::new();
    for i in 0..t.len() - 1 {
        match t[i].kind {
            Kind::LParen => stack.push(i as u32),
            Kind::RParen => {
                if let Some(o) = stack.pop() { pm[o as usize] = i as u32; pm[i] = o; }
            }
            _ => {}
        }
    }
}

pub fn reverse_parse<B: Builder>(t: &[Token], b: &mut B, st: &mut RevState<B::V>) -> Result<B::V, Error>
where B::V: Copy {
    paren_match(t, &mut st.pm);
    st.items.clear();
    st.items.reserve(t.len());
    let mut r = Rev { t, pm: &st.pm, items: &mut st.items };
    r.reduce_range(b, 0, t.len() - 1)
}

struct Rev<'a, V: Copy> {
    t: &'a [Token],
    pm: &'a [u32],
    items: &'a mut Vec<Item<V>>,
}

impl<'a, V: Copy> Rev<'a, V> {
    /// Reduce the ^/unary segment items[seg_start..] — `un* opd (^ un* opd)*`
    /// — to one operand by folding RIGHT-TO-LEFT (right-assoc ^; ^ binds
    /// tighter than a prefix sign), then pop the segment.
    fn close_seg<B: Builder<V = V>>(&mut self, b: &mut B, seg_start: usize) -> Result<V, Error> {
        let items = &mut *self.items;
        let n = items.len();
        if n == seg_start + 1 {
            if let Item::Opd(v) = items[seg_start] { items.truncate(seg_start); return Ok(v); } // lone operand
        }
        let mut acc = match items.last() {
            Some(Item::Opd(v)) if n > seg_start => *v,
            _ => return Err(Error("malformed segment".into())),
        };
        let mut i = n - 1;
        while i > seg_start {
            i -= 1;
            match items[i] {
                Item::Un(k) => acc = b.unary(k, acc),
                Item::Op(Kind::Caret) if i > seg_start => {
                    i -= 1;
                    match items[i] {
                        Item::Opd(base) => acc = b.pow(base, acc),
                        _ => return Err(Error("malformed segment".into())),
                    }
                }
                _ => return Err(Error("malformed segment".into())),
            }
        }
        items.truncate(seg_start);
        Ok(acc)
    }

    /// Left-to-right left-associative contraction of one binary level over
    /// items[base..], in place.
    fn reduce_bin_level<B: Builder<V = V>>(&mut self, b: &mut B, base: usize, k1: Kind, k2: Kind) -> Result<(), Error> {
        let items = &mut *self.items;
        let mut w = base;
        let mut r = base + 1;
        while r < items.len() {
            let (op, rhs) = (items[r], items[r + 1]);
            r += 2;
            match (op, rhs) {
                (Item::Op(k), Item::Opd(rv)) if k == k1 || k == k2 => {
                    let l = match items[w] { Item::Opd(v) => v, _ => return Err(Error("reduction failed".into())) };
                    items[w] = Item::Opd(if k1 == Kind::Star { b.mul_div(k, l, rv) } else { b.add_sub(k, l, rv) });
                }
                _ => { items[w + 1] = op; items[w + 2] = rhs; w += 2; }
            }
        }
        items.truncate(w + 1);
        Ok(())
    }

    fn reduce_range<B: Builder<V = V>>(&mut self, b: &mut B, lo: usize, hi: usize) -> Result<V, Error> {
        if lo >= hi { return Err(Error("empty sub-expression".into())); }
        let base = self.items.len();
        // 1. materialise items above `base`, recursing into parens (innermost
        //    first); a `* / + -` barrier closes the current ^/unary segment
        let mut seg_start = base;
        let mut i = lo;
        let mut expect = true;
        while i < hi {
            let tok = self.t[i];
            match tok.kind {
                Kind::LParen => {
                    let j = self.pm[i] as usize;
                    let v = self.reduce_range(b, i + 1, j)?;
                    self.items.push(Item::Opd(v)); i = j + 1; expect = false;
                }
                Kind::Num => { self.items.push(Item::Opd(b.num(tok.value))); i += 1; expect = false; }
                Kind::Ident => { self.items.push(Item::Opd(b.var(tok.value as usize))); i += 1; expect = false; }
                k @ (Kind::Plus | Kind::Minus | Kind::Star | Kind::Slash | Kind::Caret) => {
                    if expect {
                        if !matches!(k, Kind::Plus | Kind::Minus) { return Err(Error("unexpected operator".into())); }
                        self.items.push(Item::Un(k)); i += 1;
                    } else if k == Kind::Caret {
                        self.items.push(Item::Op(k)); i += 1; expect = true;
                    } else {
                        let v = self.close_seg(b, seg_start)?;
                        self.items.push(Item::Opd(v));
                        self.items.push(Item::Op(k));
                        seg_start = self.items.len();
                        i += 1; expect = true;
                    }
                }
                _ => return Err(Error("syntax error".into())),
            }
        }
        if expect { return Err(Error("unexpected end of sub-expression".into())); }
        let v = self.close_seg(b, seg_start)?;
        self.items.push(Item::Opd(v));
        // 2. contract `* /` then `+ -` (skipped when the range was one segment)
        if self.items.len() - base > 1 {
            self.reduce_bin_level(b, base, Kind::Star, Kind::Slash)?;
            self.reduce_bin_level(b, base, Kind::Plus, Kind::Minus)?;
        }
        let out = match self.items[base..] {
            [Item::Opd(v)] => v,
            _ => return Err(Error("reduction failed".into())),
        };
        self.items.truncate(base);
        Ok(out)
    }
}
