//! Fifteen math-expression parsing/evaluation strategies under one grammar,
//! one shared lexer and one spec/fuzz suite — the Rust port of the C++,
//! Python and Haskell trees. See README.md for the rules and the strategy
//! list; the drivers live in one module each.

pub mod builder;
pub mod bytecode;
pub mod classics;
pub mod fold;
pub mod lexer;
pub mod multipass;
pub mod reverse;
pub mod scannerless;

use builder::{ANode, Arena, Builder, Direct, PtrAst, Vars};
use classics::SyStacks;

/// Boxed message so `Result<Token, Error>` (and `Result<f64, Error>`) stays
/// 16 bytes and is returned in registers — the error path is cold, the Ok
/// path is every token. With an inline `String` the Result is 24 bytes and
/// goes through memory on every call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub Box<str>);

impl Error {
    #[cold]
    #[inline(never)]
    pub fn at(msg: &str, pos: u32) -> Error {
        Error(format!("{} at position {}", msg, pos).into())
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(&self.0) }
}
impl std::error::Error for Error {}

/// One strategy. Evaluators own their working buffers, so a warm evaluator
/// reuses them across calls (the C++ evaluators do the same).
pub trait Evaluator {
    fn name(&self) -> &'static str;
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error>;
}

// ---- driver × builder instantiations ---------------------------------------
macro_rules! ptr_ast {
    ($ty:ident, $name:literal, $driver:path) => {
        pub struct $ty;
        impl Evaluator for $ty {
            fn name(&self) -> &'static str { $name }
            fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
                let mut b = PtrAst { vars };
                let root = $driver(src, &mut b)?;
                Ok(b.result(root))
            }
        }
    };
}
macro_rules! direct {
    ($ty:ident, $name:literal, $driver:path) => {
        pub struct $ty;
        impl Evaluator for $ty {
            fn name(&self) -> &'static str { $name }
            fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
                let mut b = Direct { vars };
                let root = $driver(src, &mut b)?;
                Ok(b.result(root))
            }
        }
    };
}

ptr_ast!(AstRd, "ast-recursive-descent", classics::rd_parse);
ptr_ast!(AstPratt, "ast-pratt", classics::pratt_parse);
direct!(DirectRd, "direct-recursive-descent", classics::rd_parse);

pub struct AstSy(SyStacks<Box<builder::Node>>);
impl Evaluator for AstSy {
    fn name(&self) -> &'static str { "ast-shunting-yard" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        let mut b = PtrAst { vars };
        let root = classics::sy_parse(src, &mut b, &mut self.0)?;
        Ok(b.result(root))
    }
}
pub struct DirectSy(SyStacks<f64>);
impl Evaluator for DirectSy {
    fn name(&self) -> &'static str { "direct-shunting-yard" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        let mut b = Direct { vars };
        let root = classics::sy_parse(src, &mut b, &mut self.0)?;
        Ok(b.result(root))
    }
}

/// Arena-backed strategies keep the node vector warm across evals.
pub struct AstArena { nodes: Vec<ANode> }
impl Evaluator for AstArena {
    fn name(&self) -> &'static str { "ast-arena" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        self.nodes.clear();
        let mut b = Arena { vars, nodes: &mut self.nodes };
        let root = classics::rd_parse(src, &mut b)?;
        Ok(b.result(root))
    }
}

pub struct Multipass { st: multipass::MpState }
impl Evaluator for Multipass {
    fn name(&self) -> &'static str { "multipass" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        let toks = lexer::tokenize(src)?;
        let mut b = PtrAst { vars };
        let root = multipass::mp_parse(&toks, &mut b, &mut self.st, false)?;
        Ok(b.result(root))
    }
}
pub struct MultipassArena { st: multipass::MpState, nodes: Vec<ANode> }
impl Evaluator for MultipassArena {
    fn name(&self) -> &'static str { "multipass-arena" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        let toks = lexer::tokenize(src)?;
        self.nodes.clear();
        let mut b = Arena { vars, nodes: &mut self.nodes };
        let root = multipass::mp_parse(&toks, &mut b, &mut self.st, false)?;
        Ok(b.result(root))
    }
}
pub struct DirectMp { st: multipass::MpState }
impl Evaluator for DirectMp {
    fn name(&self) -> &'static str { "direct-mp" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        let toks = lexer::tokenize(src)?;
        let mut b = Direct { vars };
        let root = multipass::mp_parse(&toks, &mut b, &mut self.st, false)?;
        Ok(b.result(root))
    }
}
pub struct MultipassBfs { st: multipass::MpState, nodes: Vec<ANode> }
impl Evaluator for MultipassBfs {
    fn name(&self) -> &'static str { "multipass-bfs" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        let toks = lexer::tokenize(src)?;
        self.nodes.clear();
        let mut b = Arena { vars, nodes: &mut self.nodes };
        let root = multipass::mp_parse(&toks, &mut b, &mut self.st, true)?;
        Ok(b.result(root))
    }
}

pub struct MultipassReverse { st: reverse::RevState<i32>, nodes: Vec<ANode> }
impl Evaluator for MultipassReverse {
    fn name(&self) -> &'static str { "multipass-reverse" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        let toks = lexer::tokenize(src)?;
        self.nodes.clear();
        let mut b = Arena { vars, nodes: &mut self.nodes };
        let root = reverse::reverse_parse(&toks, &mut b, &mut self.st)?;
        Ok(b.result(root))
    }
}

pub struct ReverseFold { st: fold::FoldState<i32>, nodes: Vec<ANode> }
impl Evaluator for ReverseFold {
    fn name(&self) -> &'static str { "multipass-reverse-fold" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        self.nodes.clear();
        self.nodes.reserve(src.len() + 1);
        let mut b = Arena { vars, nodes: &mut self.nodes };
        let root = fold::fold_parse(src, &mut b, &mut self.st)?;
        Ok(b.result(root))
    }
}
pub struct DirectReverse { st: fold::FoldState<f64> }
impl Evaluator for DirectReverse {
    fn name(&self) -> &'static str { "direct-reverse" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        let mut b = Direct { vars };
        fold::fold_parse(src, &mut b, &mut self.st)
    }
}

pub struct Scannerless;
impl Evaluator for Scannerless {
    fn name(&self) -> &'static str { "direct-scannerless" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> { scannerless::scannerless_eval(src, vars) }
}

pub struct BytecodeVm { st: bytecode::VmState }
impl Evaluator for BytecodeVm {
    fn name(&self) -> &'static str { "bytecode-vm" }
    fn eval(&mut self, src: &str, vars: Vars) -> Result<f64, Error> {
        bytecode::compile(src, &mut self.st)?;
        bytecode::run(&mut self.st, vars)
    }
}

/// Every strategy, in the registry order shared by all four languages.
pub fn all_evaluators() -> Vec<Box<dyn Evaluator>> {
    vec![
        Box::new(AstRd),
        Box::new(AstSy(SyStacks::default())),
        Box::new(AstPratt),
        Box::new(AstArena { nodes: Vec::new() }),
        Box::new(Multipass { st: Default::default() }),
        Box::new(MultipassArena { st: Default::default(), nodes: Vec::new() }),
        Box::new(DirectMp { st: Default::default() }),
        Box::new(MultipassBfs { st: Default::default(), nodes: Vec::new() }),
        Box::new(MultipassReverse { st: reverse::RevState::new(), nodes: Vec::new() }),
        Box::new(ReverseFold { st: Default::default(), nodes: Vec::new() }),
        Box::new(DirectRd),
        Box::new(DirectSy(SyStacks::default())),
        Box::new(DirectReverse { st: Default::default() }),
        Box::new(Scannerless),
        Box::new(BytecodeVm { st: Default::default() }),
    ]
}

/// Run `f` on a thread with a large stack: the recursive strategies descend
/// once per nesting level, and nestchain at 8 192 leaves is 8 192 deep.
pub fn with_big_stack<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> T {
    std::thread::Builder::new()
        .stack_size(512 * 1024 * 1024)
        .spawn(f)
        .expect("spawn")
        .join()
        .expect("join")
}
