//! bytecode-vm: shunting-yard compiles the stream to a flat opcode vector,
//! a stack machine runs it. Compile + run per eval, buffers reused.

use crate::builder::{apply, lookup, op_of, Op, Vars};
use crate::classics::{bin_prec, UNARY_PREC};
use crate::lexer::{Kind, Lexer};
use crate::Error;

#[derive(Clone, Copy)]
pub enum Ins {
    Push(f64),
    Load(u8),
    Neg,
    Bin(Op),
}

#[derive(Clone, Copy)]
struct OpEnt { kind: Kind, prec: u8, unary: bool, lparen: bool }

#[derive(Default)]
pub struct VmState {
    code: Vec<Ins>,
    ops: Vec<OpEnt>,
    stack: Vec<f64>,
}

fn emit(code: &mut Vec<Ins>, op: OpEnt) -> Result<(), Error> {
    if op.lparen { return Err(Error("mismatched parenthesis".into())); }
    if op.unary {
        if op.kind == Kind::Minus { code.push(Ins::Neg); } // unary plus: no opcode
    } else {
        code.push(Ins::Bin(op_of(op.kind)));
    }
    Ok(())
}

pub fn compile(src: &str, st: &mut VmState) -> Result<(), Error> {
    st.code.clear();
    st.ops.clear();
    let code = &mut st.code;
    let ops = &mut st.ops;
    let mut lx = Lexer::new(src);
    // Two-mode scan, as in classics::sy_parse.
    'scan: loop {
        loop {
            let t = lx.next()?;
            match t.kind {
                Kind::Num => { code.push(Ins::Push(t.value)); break; }
                Kind::Ident => { code.push(Ins::Load(t.value as u8)); break; }
                Kind::LParen => ops.push(OpEnt { kind: Kind::LParen, prec: 0, unary: false, lparen: true }),
                Kind::Plus | Kind::Minus => ops.push(OpEnt { kind: t.kind, prec: UNARY_PREC, unary: true, lparen: false }),
                Kind::End => return Err(Error::at("unexpected end of input", t.pos)),
                Kind::RParen => return Err(Error::at("empty parentheses", t.pos)),
                _ => return Err(Error::at("unexpected operator", t.pos)),
            }
        }
        loop {
            let t = lx.next()?;
            match t.kind {
                Kind::Plus | Kind::Minus | Kind::Star | Kind::Slash | Kind::Caret => {
                    let prec = bin_prec(t.kind);
                    let ra = t.kind == Kind::Caret;
                    while let Some(&top) = ops.last() {
                        if top.lparen || !(top.prec > prec || (top.prec == prec && !ra)) { break; }
                        ops.pop();
                        emit(code, top)?;
                    }
                    ops.push(OpEnt { kind: t.kind, prec, unary: false, lparen: false });
                    break;
                }
                Kind::RParen => loop {
                    match ops.pop() {
                        None => return Err(Error::at("mismatched parenthesis", t.pos)),
                        Some(o) if o.lparen => break,
                        Some(o) => emit(code, o)?,
                    }
                },
                Kind::End => break 'scan,
                Kind::Num => return Err(Error::at("unexpected number", t.pos)),
                Kind::Ident => return Err(Error::at("unexpected variable", t.pos)),
                Kind::LParen => return Err(Error::at("unexpected '('", t.pos)),
            }
        }
    }
    while let Some(o) = ops.pop() { emit(code, o)?; }
    Ok(())
}

pub fn run(st: &mut VmState, vars: Vars) -> Result<f64, Error> {
    let stack = &mut st.stack;
    stack.clear();
    for ins in &st.code {
        match *ins {
            Ins::Push(v) => stack.push(v),
            Ins::Load(i) => stack.push(lookup(vars, i as usize)),
            Ins::Neg => {
                let x = stack.pop().ok_or_else(|| Error("stack underflow".into()))?;
                stack.push(-x);
            }
            Ins::Bin(op) => {
                let r = stack.pop().ok_or_else(|| Error("stack underflow".into()))?;
                let l = stack.pop().ok_or_else(|| Error("stack underflow".into()))?;
                stack.push(apply(op, l, r));
            }
        }
    }
    if stack.len() != 1 { return Err(Error("invalid expression".into())); }
    Ok(stack[0])
}
