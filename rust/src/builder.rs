//! The representation axis. A driver (parse order) is written once, generic
//! over a `Builder`, and instantiated at three carriers — the same
//! factoring as the Python port and the C++ policy templates:
//!
//!   PtrAst   — `Box`ed tree nodes (one allocation per node)   ast-*, multipass
//!   Arena    — one `Vec` of nodes, children by index          ast-arena, multipass-arena/-bfs, the reverse forms
//!   Direct   — no tree, every node collapses to an f64        direct-*

use crate::lexer::Kind;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Op {
    Add,
    Sub,
    Mul,
    Div,
    Pow,
}

#[inline]
pub fn op_of(k: Kind) -> Op {
    match k {
        Kind::Plus => Op::Add,
        Kind::Minus => Op::Sub,
        Kind::Star => Op::Mul,
        Kind::Slash => Op::Div,
        _ => Op::Pow,
    }
}

/// IEEE semantics throughout: x/0 = ±inf, 0/0 = nan, `powf` is libm pow —
/// bit-identical to C++ `std::pow`, Python's patched `math.pow`, GHC's `**`.
#[inline]
pub fn apply(op: Op, l: f64, r: f64) -> f64 {
    match op {
        Op::Add => l + r,
        Op::Sub => l - r,
        Op::Mul => l * r,
        Op::Div => l / r,
        Op::Pow => l.powf(r),
    }
}

pub type Vars<'a> = Option<&'a [f64; 26]>;

#[inline]
pub fn lookup(vars: Vars, i: usize) -> f64 {
    match vars {
        Some(v) => v[i],
        None => 0.0,
    }
}

pub trait Builder {
    type V;
    fn num(&mut self, v: f64) -> Self::V;
    fn var(&mut self, i: usize) -> Self::V;
    fn neg(&mut self, a: Self::V) -> Self::V;
    fn pos(&mut self, a: Self::V) -> Self::V;
    fn binop(&mut self, op: Op, l: Self::V, r: Self::V) -> Self::V;
    /// Collapse the root to a value against the variable environment.
    fn result(&mut self, root: Self::V) -> f64;

    // Level-aware forms for drivers that already know which precedence level
    // they are reducing (the fused fold, the buffered reverse): one compare
    // instead of a five-way dispatch on a random operator. Same interface
    // the C++ fold's policy has (mulDiv / addSub / pow / unary); defaults
    // route through `binop`.
    #[inline]
    fn mul_div(&mut self, k: Kind, l: Self::V, r: Self::V) -> Self::V { self.binop(op_of(k), l, r) }
    #[inline]
    fn add_sub(&mut self, k: Kind, l: Self::V, r: Self::V) -> Self::V { self.binop(op_of(k), l, r) }
    #[inline]
    fn pow(&mut self, l: Self::V, r: Self::V) -> Self::V { self.binop(Op::Pow, l, r) }
    #[inline]
    fn unary(&mut self, k: Kind, a: Self::V) -> Self::V { if k == Kind::Minus { self.neg(a) } else { self.pos(a) } }
}

// ---- pointer AST -----------------------------------------------------------
pub enum Node {
    Num(f64),
    Var(u8),
    Neg(Box<Node>),
    Pos(Box<Node>),
    Bin(Op, Box<Node>, Box<Node>),
}

fn eval_node(n: &Node, vars: Vars) -> f64 {
    match n {
        Node::Num(v) => *v,
        Node::Var(i) => lookup(vars, *i as usize),
        Node::Neg(a) => -eval_node(a, vars),
        Node::Pos(a) => eval_node(a, vars),
        Node::Bin(op, l, r) => apply(*op, eval_node(l, vars), eval_node(r, vars)),
    }
}

pub struct PtrAst<'a> {
    pub vars: Vars<'a>,
}

impl<'a> Builder for PtrAst<'a> {
    type V = Box<Node>;
    #[inline]
    fn num(&mut self, v: f64) -> Box<Node> { Box::new(Node::Num(v)) }
    #[inline]
    fn var(&mut self, i: usize) -> Box<Node> { Box::new(Node::Var(i as u8)) }
    #[inline]
    fn neg(&mut self, a: Box<Node>) -> Box<Node> { Box::new(Node::Neg(a)) }
    #[inline]
    fn pos(&mut self, a: Box<Node>) -> Box<Node> { Box::new(Node::Pos(a)) }
    #[inline]
    fn binop(&mut self, op: Op, l: Box<Node>, r: Box<Node>) -> Box<Node> { Box::new(Node::Bin(op, l, r)) }
    fn result(&mut self, root: Box<Node>) -> f64 { eval_node(&root, self.vars) }
}

// ---- arena AST -------------------------------------------------------------
#[derive(Clone, Copy)]
#[repr(u8)]
pub enum K {
    Num,
    Var,
    Neg,
    Pos,
    Add,
    Sub,
    Mul,
    Div,
    Pow,
}

#[derive(Clone, Copy)]
pub struct ANode {
    pub kind: K,
    pub a: i32,
    pub b: i32,
    pub value: f64, // literal for Num; variable index (as f64) for Var
}

/// The node vector lives in the evaluator and is reused across evals (warm),
/// like the C++ arena strategies.
pub struct Arena<'a> {
    pub vars: Vars<'a>,
    pub nodes: &'a mut Vec<ANode>,
}

impl<'a> Arena<'a> {
    #[inline]
    fn emit(&mut self, n: ANode) -> i32 {
        self.nodes.push(n);
        (self.nodes.len() - 1) as i32
    }
    fn walk(&self, i: i32) -> f64 {
        let n = self.nodes[i as usize];
        match n.kind {
            K::Num => n.value,
            K::Var => lookup(self.vars, n.a as usize),
            K::Neg => -self.walk(n.a),
            K::Pos => self.walk(n.a),
            K::Add => self.walk(n.a) + self.walk(n.b),
            K::Sub => self.walk(n.a) - self.walk(n.b),
            K::Mul => self.walk(n.a) * self.walk(n.b),
            K::Div => self.walk(n.a) / self.walk(n.b),
            K::Pow => self.walk(n.a).powf(self.walk(n.b)),
        }
    }
}

impl<'a> Builder for Arena<'a> {
    type V = i32;
    #[inline]
    fn num(&mut self, v: f64) -> i32 { self.emit(ANode { kind: K::Num, a: -1, b: -1, value: v }) }
    #[inline]
    fn var(&mut self, i: usize) -> i32 { self.emit(ANode { kind: K::Var, a: i as i32, b: -1, value: 0.0 }) }
    #[inline]
    fn neg(&mut self, a: i32) -> i32 { self.emit(ANode { kind: K::Neg, a, b: -1, value: 0.0 }) }
    #[inline]
    fn pos(&mut self, a: i32) -> i32 { self.emit(ANode { kind: K::Pos, a, b: -1, value: 0.0 }) }
    #[inline]
    fn binop(&mut self, op: Op, l: i32, r: i32) -> i32 {
        let kind = match op {
            Op::Add => K::Add,
            Op::Sub => K::Sub,
            Op::Mul => K::Mul,
            Op::Div => K::Div,
            Op::Pow => K::Pow,
        };
        self.emit(ANode { kind, a: l, b: r, value: 0.0 })
    }
    fn result(&mut self, root: i32) -> f64 { self.walk(root) }
    #[inline]
    fn mul_div(&mut self, k: Kind, l: i32, r: i32) -> i32 {
        self.emit(ANode { kind: if k == Kind::Star { K::Mul } else { K::Div }, a: l, b: r, value: 0.0 })
    }
    #[inline]
    fn add_sub(&mut self, k: Kind, l: i32, r: i32) -> i32 {
        self.emit(ANode { kind: if k == Kind::Plus { K::Add } else { K::Sub }, a: l, b: r, value: 0.0 })
    }
    #[inline]
    fn pow(&mut self, l: i32, r: i32) -> i32 { self.emit(ANode { kind: K::Pow, a: l, b: r, value: 0.0 }) }
    #[inline]
    fn unary(&mut self, k: Kind, a: i32) -> i32 {
        self.emit(ANode { kind: if k == Kind::Minus { K::Neg } else { K::Pos }, a, b: -1, value: 0.0 })
    }
}

// ---- direct ----------------------------------------------------------------
pub struct Direct<'a> {
    pub vars: Vars<'a>,
}

impl<'a> Builder for Direct<'a> {
    type V = f64;
    #[inline]
    fn num(&mut self, v: f64) -> f64 { v }
    #[inline]
    fn var(&mut self, i: usize) -> f64 { lookup(self.vars, i) }
    #[inline]
    fn neg(&mut self, a: f64) -> f64 { -a }
    #[inline]
    fn pos(&mut self, a: f64) -> f64 { a }
    #[inline]
    fn binop(&mut self, op: Op, l: f64, r: f64) -> f64 { apply(op, l, r) }
    #[inline]
    fn result(&mut self, root: f64) -> f64 { root }
    #[inline]
    fn mul_div(&mut self, k: Kind, l: f64, r: f64) -> f64 { if k == Kind::Star { l * r } else { l / r } }
    #[inline]
    fn add_sub(&mut self, k: Kind, l: f64, r: f64) -> f64 { if k == Kind::Plus { l + r } else { l - r } }
    #[inline]
    fn pow(&mut self, l: f64, r: f64) -> f64 { l.powf(r) }
    #[inline]
    fn unary(&mut self, k: Kind, a: f64) -> f64 { if k == Kind::Minus { -a } else { a } }
}
