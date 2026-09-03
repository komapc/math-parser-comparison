//! The three left-to-right classics — recursive descent, Pratt, shunting-yard —
//! written once over the streaming lexer and instantiated per builder.

use crate::builder::{op_of, Builder, Op};
use crate::lexer::{Kind, Lexer, Token};
use crate::Error;

pub const UNARY_PREC: u8 = 3;

#[inline]
pub fn bin_prec(k: Kind) -> u8 {
    match k {
        Kind::Plus | Kind::Minus => 1,
        Kind::Star | Kind::Slash => 2,
        Kind::Caret => 4,
        _ => 0,
    }
}

/// One token of lookahead over the stream.
pub struct Stream<'a> {
    lx: Lexer<'a>,
    pub cur: Token,
}

impl<'a> Stream<'a> {
    #[inline]
    pub fn new(src: &'a str) -> Result<Self, Error> {
        let mut lx = Lexer::new(src);
        let cur = lx.next()?;
        Ok(Stream { lx, cur })
    }
    #[inline]
    pub fn advance(&mut self) -> Result<Token, Error> {
        let t = self.cur;
        self.cur = self.lx.next()?;
        Ok(t)
    }
    #[inline]
    pub fn kind(&self) -> Kind {
        self.cur.kind
    }
}

// ---- recursive descent -----------------------------------------------------
pub fn rd_parse<B: Builder>(src: &str, b: &mut B) -> Result<B::V, Error> {
    let mut s = Stream::new(src)?;
    let root = rd_expr(&mut s, b)?;
    if s.kind() != Kind::End {
        return Err(Error::at("unexpected token", s.cur.pos));
    }
    Ok(root)
}

fn rd_expr<B: Builder>(s: &mut Stream, b: &mut B) -> Result<B::V, Error> {
    let mut left = rd_term(s, b)?;
    while matches!(s.kind(), Kind::Plus | Kind::Minus) {
        let op = op_of(s.advance()?.kind);
        let r = rd_term(s, b)?;
        left = b.binop(op, left, r);
    }
    Ok(left)
}

fn rd_term<B: Builder>(s: &mut Stream, b: &mut B) -> Result<B::V, Error> {
    let mut left = rd_unary(s, b)?;
    while matches!(s.kind(), Kind::Star | Kind::Slash) {
        let op = op_of(s.advance()?.kind);
        let r = rd_unary(s, b)?;
        left = b.binop(op, left, r);
    }
    Ok(left)
}

fn rd_unary<B: Builder>(s: &mut Stream, b: &mut B) -> Result<B::V, Error> {
    match s.kind() {
        Kind::Minus => {
            s.advance()?;
            let c = rd_unary(s, b)?;
            Ok(b.neg(c))
        }
        Kind::Plus => {
            s.advance()?;
            let c = rd_unary(s, b)?;
            Ok(b.pos(c))
        }
        _ => rd_power(s, b),
    }
}

fn rd_power<B: Builder>(s: &mut Stream, b: &mut B) -> Result<B::V, Error> {
    let base = rd_primary(s, b)?;
    if s.kind() == Kind::Caret {
        s.advance()?;
        let e = rd_unary(s, b)?;
        return Ok(b.binop(Op::Pow, base, e));
    }
    Ok(base)
}

fn rd_primary<B: Builder>(s: &mut Stream, b: &mut B) -> Result<B::V, Error> {
    let t = s.cur;
    match t.kind {
        Kind::Num => {
            s.advance()?;
            Ok(b.num(t.value))
        }
        Kind::Ident => {
            s.advance()?;
            Ok(b.var(t.value as usize))
        }
        Kind::LParen => {
            s.advance()?;
            let e = rd_expr(s, b)?;
            if s.kind() != Kind::RParen {
                return Err(Error::at("expected ')'", s.cur.pos));
            }
            s.advance()?;
            Ok(e)
        }
        _ => Err(Error::at("expected number or '('", t.pos)),
    }
}

// ---- Pratt / precedence climbing --------------------------------------------
pub fn pratt_parse<B: Builder>(src: &str, b: &mut B) -> Result<B::V, Error> {
    let mut s = Stream::new(src)?;
    let root = pratt(&mut s, b, 0)?;
    if s.kind() != Kind::End {
        return Err(Error::at("unexpected token", s.cur.pos));
    }
    Ok(root)
}

fn pratt<B: Builder>(s: &mut Stream, b: &mut B, rbp: u8) -> Result<B::V, Error> {
    let t = s.advance()?;
    let mut left = match t.kind {
        Kind::Num => b.num(t.value),
        Kind::Ident => b.var(t.value as usize),
        Kind::Minus => {
            let o = pratt(s, b, UNARY_PREC)?; // rbp 3 < ^ lbp 4: a prefix sign grabs a power
            b.neg(o)
        }
        Kind::Plus => {
            let o = pratt(s, b, UNARY_PREC)?;
            b.pos(o)
        }
        Kind::LParen => {
            let e = pratt(s, b, 0)?;
            if s.kind() != Kind::RParen {
                return Err(Error::at("expected ')'", s.cur.pos));
            }
            s.advance()?;
            e
        }
        _ => return Err(Error::at("unexpected token", t.pos)),
    };
    loop {
        let k = s.kind();
        let lb = bin_prec(k);
        if lb <= rbp {
            return Ok(left);
        }
        s.advance()?;
        let right = pratt(s, b, if k == Kind::Caret { lb - 1 } else { lb })?;
        left = b.binop(op_of(k), left, right);
    }
}

// ---- shunting-yard ---------------------------------------------------------
#[derive(Clone, Copy)]
struct SyOp {
    kind: Kind,
    prec: u8,
    right_assoc: bool,
    unary: bool,
    lparen: bool,
}

/// The two stacks live in the evaluator and are reused across evals.
pub struct SyStacks<V> {
    pub out: Vec<V>,
    ops: Vec<SyOp>,
}

impl<V> Default for SyStacks<V> {
    fn default() -> Self {
        SyStacks { out: Vec::new(), ops: Vec::new() }
    }
}

pub fn sy_parse<B: Builder>(src: &str, b: &mut B, st: &mut SyStacks<B::V>) -> Result<B::V, Error> {
    st.out.clear();
    st.ops.clear();
    let mut lx = Lexer::new(src);
    let mut expect_operand = true;
    loop {
        let t = lx.next()?;
        match t.kind {
            Kind::Num => {
                if !expect_operand {
                    return Err(Error::at("unexpected number", t.pos));
                }
                let v = b.num(t.value);
                st.out.push(v);
                expect_operand = false;
            }
            Kind::Ident => {
                if !expect_operand {
                    return Err(Error::at("unexpected variable", t.pos));
                }
                let v = b.var(t.value as usize);
                st.out.push(v);
                expect_operand = false;
            }
            Kind::LParen => {
                if !expect_operand {
                    return Err(Error::at("unexpected '('", t.pos));
                }
                st.ops.push(SyOp { kind: Kind::LParen, prec: 0, right_assoc: false, unary: false, lparen: true });
                expect_operand = true;
            }
            Kind::RParen => {
                if expect_operand {
                    return Err(Error::at("empty parentheses", t.pos));
                }
                loop {
                    match st.ops.pop() {
                        None => return Err(Error::at("mismatched parenthesis", t.pos)),
                        Some(op) if op.lparen => break,
                        Some(op) => sy_emit(b, &mut st.out, op)?,
                    }
                }
                expect_operand = false;
            }
            Kind::Plus | Kind::Minus | Kind::Star | Kind::Slash | Kind::Caret => {
                if expect_operand {
                    if !matches!(t.kind, Kind::Plus | Kind::Minus) {
                        return Err(Error::at("unexpected operator", t.pos));
                    }
                    st.ops.push(SyOp { kind: t.kind, prec: UNARY_PREC, right_assoc: true, unary: true, lparen: false });
                } else {
                    let prec = bin_prec(t.kind);
                    let ra = t.kind == Kind::Caret;
                    while let Some(&top) = st.ops.last() {
                        if top.lparen || !(top.prec > prec || (top.prec == prec && !ra)) {
                            break;
                        }
                        st.ops.pop();
                        sy_emit(b, &mut st.out, top)?;
                    }
                    st.ops.push(SyOp { kind: t.kind, prec, right_assoc: ra, unary: false, lparen: false });
                    expect_operand = true;
                }
            }
            Kind::End => {
                if expect_operand {
                    return Err(Error::at("unexpected end of input", t.pos));
                }
                break;
            }
        }
    }
    while let Some(op) = st.ops.pop() {
        sy_emit(b, &mut st.out, op)?;
    }
    if st.out.len() != 1 {
        return Err(Error("invalid expression".into()));
    }
    Ok(st.out.pop().unwrap())
}

#[inline]
fn sy_emit<B: Builder>(b: &mut B, out: &mut Vec<B::V>, op: SyOp) -> Result<(), Error> {
    if op.lparen {
        return Err(Error("mismatched parenthesis".into()));
    }
    if op.unary {
        let x = out.pop().ok_or_else(|| Error("invalid expression".into()))?;
        out.push(if op.kind == Kind::Minus { b.neg(x) } else { b.pos(x) });
    } else {
        let r = out.pop().ok_or_else(|| Error("invalid expression".into()))?;
        let l = out.pop().ok_or_else(|| Error("invalid expression".into()))?;
        out.push(b.binop(op_of(op.kind), l, r));
    }
    let _ = op.right_assoc;
    Ok(())
}
