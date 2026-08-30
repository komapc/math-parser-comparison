{-# LANGUAGE RankNTypes #-}
{-# LANGUAGE BangPatterns #-}
{-# LANGUAGE FlexibleContexts #-}

-- | The fourteen strategies, idiomatic Haskell.
--
-- Representation is abstracted tagless-final via the 'Sym' class, so each parse
-- algorithm is written once and instantiated at three carriers:
--
--   * 'Expr'   — the algebraic data type (pointer-AST analog)   -> ast-*, multipass
--   * 'Arena'  — a state-threaded flat node array               -> ast-arena, multipass-arena/-bfs
--   * 'Direct' — an @Env -> Double@ closure (no tree)           -> direct-*
--
-- bytecode-vm is a separate compile-to-instructions-then-run pass.
module MathParser.Strategies
  ( Evaluator(..)
  , Env
  , allEvaluators
  , reevalForms
  ) where

import           Data.Array          hiding ((!), bounds, listArray)
import           Data.Array.Unboxed  (UArray, (!), bounds, listArray)
import           Data.Bits (countLeadingZeros, finiteBitSize)
import           Data.List (foldl')
import qualified Data.IntMap.Strict as IM

import           MathParser.Lexer

-- ---- semantics -------------------------------------------------------------
data Op = Add | Sub | Mul | Div | Pow deriving (Eq, Show)

opOf :: Kind -> Op
opOf KPlus  = Add
opOf KMinus = Sub
opOf KStar  = Mul
opOf KSlash = Div
opOf KCaret = Pow
opOf k      = error ("not a binary operator: " ++ show k)

binPrec :: Kind -> Int
binPrec KPlus  = 1
binPrec KMinus = 1
binPrec KStar  = 2
binPrec KSlash = 2
binPrec KCaret = 4
binPrec _      = -1

applyOp :: Op -> Double -> Double -> Double
applyOp Add a b = a + b
applyOp Sub a b = a - b
applyOp Mul a b = a * b
applyOp Div a b = a / b          -- IEEE: x/0 = inf, 0/0 = nan (matches C++)
applyOp Pow a b = applyPow a b

-- GHC's @**@ on Double is libm pow, so it already matches C++ std::pow
-- bit-for-bit — including negative bases with integral exponents
-- ((-2)**3 = -8) and the IEEE corners (0**(-1) = inf). An earlier version
-- special-cased integral exponents through @^^@, whose exponentiation by
-- squaring rounds differently from libm (1 ULP off on e.g. 3^34).
applyPow :: Double -> Double -> Double
applyPow = (**)

type Env = Int -> Double

-- ---- tagless-final representation class ------------------------------------
class Sym r where
  sNum :: Double -> r
  sVar :: Int -> r
  sNeg :: r -> r
  sPos :: r -> r
  sPos = id               -- unary plus is identity in every carrier
  sBin :: Op -> r -> r -> r

-- pointer-AST carrier
data Expr = ENum !Double | EVar !Int | ENeg Expr | EBin !Op Expr Expr

instance Sym Expr where
  sNum = ENum
  sVar = EVar
  sNeg = ENeg
  sBin = EBin

evalExpr :: Env -> Expr -> Double
evalExpr env = go
  where
    go (ENum x)     = x
    go (EVar i)     = env i
    go (ENeg e)     = negate (go e)
    go (EBin op a b) = applyOp op (go a) (go b)

-- direct carrier: an environment-indexed value, no tree walk
newtype Direct = Direct { runDirect :: Env -> Double }

instance Sym Direct where
  sNum x = Direct (const x)
  sVar i = Direct (\e -> e i)
  sNeg (Direct f) = Direct (negate . f)
  sBin op (Direct a) (Direct b) = Direct (\e -> applyOp op (a e) (b e))

-- arena carrier: thread (next-index, nodes-in-reverse); a node refs children by index
data Node = NNum !Double | NVar !Int | NNeg !Int | NBin !Op !Int !Int
type ArenaSt = (Int, [Node])
newtype Arena = Arena { runArena :: ArenaSt -> (Int, ArenaSt) }

emitNode :: Node -> ArenaSt -> (Int, ArenaSt)
emitNode nd (n, xs) = (n, (n + 1, nd : xs))

instance Sym Arena where
  sNum x = Arena (emitNode (NNum x))
  sVar i = Arena (emitNode (NVar i))
  sNeg (Arena a) = Arena $ \st -> let (ai, st1) = a st in emitNode (NNeg ai) st1
  sBin op (Arena l) (Arena r) = Arena $ \st ->
    let (li, st1) = l st
        (ri, st2) = r st1
    in emitNode (NBin op li ri) st2

evalArena :: Env -> Arena -> Double
evalArena env (Arena f) =
  let (root, (cnt, xs)) = f (0, [])
      arr = listArray (0, cnt - 1) (reverse xs) :: Array Int Node
      go i = case arr ! i of
        NNum x      -> x
        NVar v      -> env v
        NNeg c      -> negate (go c)
        NBin op l r -> applyOp op (go l) (go r)
  in if cnt == 0 then error "empty expression" else go root

-- ---- driver: recursive descent ---------------------------------------------
rdParse :: Sym r => [Tok] -> r
rdParse ts = case pExpr ts of
  (e, rest) | tKind (head rest) == KEnd -> e
            | otherwise -> error "unexpected token"

pExpr :: Sym r => [Tok] -> (r, [Tok])
pExpr ts = loop l0 r0
  where
    (l0, r0) = pTerm ts
    loop l (t : rest)
      | tKind t == KPlus  = let (rt, r2) = pTerm rest in loop (sBin Add l rt) r2
      | tKind t == KMinus = let (rt, r2) = pTerm rest in loop (sBin Sub l rt) r2
    loop l toks = (l, toks)

pTerm :: Sym r => [Tok] -> (r, [Tok])
pTerm ts = loop l0 r0
  where
    (l0, r0) = pUnary ts
    loop l (t : rest)
      | tKind t == KStar  = let (rt, r2) = pUnary rest in loop (sBin Mul l rt) r2
      | tKind t == KSlash = let (rt, r2) = pUnary rest in loop (sBin Div l rt) r2
    loop l toks = (l, toks)

pUnary :: Sym r => [Tok] -> (r, [Tok])
pUnary (t : rest)
  | tKind t == KPlus  = let (c, r) = pUnary rest in (sPos c, r)
  | tKind t == KMinus = let (c, r) = pUnary rest in (sNeg c, r)
pUnary ts = pPower ts

pPower :: Sym r => [Tok] -> (r, [Tok])
pPower ts =
  let (base, r) = pPrimary ts
  in case r of
       (t : rest) | tKind t == KCaret -> let (e, r2) = pUnary rest in (sBin Pow base e, r2)
       _ -> (base, r)

pPrimary :: Sym r => [Tok] -> (r, [Tok])
pPrimary (t : rest) = case tKind t of
  KNum    -> (sNum (tVal t), rest)
  KVar    -> (sVar (round (tVal t)), rest)
  KLParen -> let (e, r) = pExpr rest
             in case r of
                  (t2 : r2) | tKind t2 == KRParen -> (e, r2)
                  _ -> error "expected ')'"
  _ -> error "expected number or '('"
pPrimary [] = error "unexpected end of input"

-- ---- driver: Pratt / precedence climbing -----------------------------------
prattParse :: Sym r => [Tok] -> r
prattParse ts = case pp 0 ts of
  (e, rest) | tKind (head rest) == KEnd -> e
            | otherwise -> error "unexpected token"

lbp :: Kind -> Int
lbp k = let p = binPrec k in if p > 0 then p else 0

pp :: Sym r => Int -> [Tok] -> (r, [Tok])
pp rbp (t : ts) = loop left ts1
  where
    (left, ts1) = nud t ts
    loop l toks@(o : rest)
      | lb <= rbp = (l, toks)
      | otherwise = let (right, ts2) = pp (if ra then lb - 1 else lb) rest
                    in loop (sBin (opOf k) l right) ts2
      where k = tKind o; lb = lbp k; ra = k == KCaret
    loop l [] = (l, [])
pp _ [] = error "unexpected end of input"

nud :: Sym r => Tok -> [Tok] -> (r, [Tok])
nud t ts = case tKind t of
  KNum    -> (sNum (tVal t), ts)
  KVar    -> (sVar (round (tVal t)), ts)
  KPlus   -> let (o, r) = pp 3 ts in (sPos o, r)   -- rbp 3 < ^ lbp 4: prefix grabs a power
  KMinus  -> let (o, r) = pp 3 ts in (sNeg o, r)
  KLParen -> let (e, r) = pp 0 ts
             in case r of
                  (t2 : r2) | tKind t2 == KRParen -> (e, r2)
                  _ -> error "expected ')'"
  _ -> error "unexpected token"

-- ---- driver: shunting-yard -------------------------------------------------
-- Operator-stack entry: kind, precedence, right-assoc, unary, is-'('.
data OpE = OpE !Kind !Int !Bool !Bool !Bool

isLP :: OpE -> Bool
isLP (OpE _ _ _ _ lp) = lp

oePrec :: OpE -> Int
oePrec (OpE _ p _ _ _) = p

syParse :: Sym r => [Tok] -> r
syParse toks = finish (go toks [] [] True)
  where
    applyE (OpE k _ _ unary lp) out
      | lp = error "mismatched parenthesis"
      | unary = case out of
          (x : rest) -> (if k == KMinus then sNeg x else sPos x) : rest
          _ -> error "missing operand"
      | otherwise = case out of
          (r : l : rest) -> sBin (opOf k) l r : rest
          _ -> error "missing operand"

    popUntilLP out (o : ops)
      | not (isLP o) = popUntilLP (applyE o out) ops
      | otherwise    = (out, ops)
    popUntilLP _ [] = error "mismatched parenthesis"

    popGE out (o : ops) prec ra
      | not (isLP o) && (oePrec o > prec || (oePrec o == prec && not ra))
        = popGE (applyE o out) ops prec ra
    popGE out ops _ _ = (out, ops)

    go (t : ts) out ops expect = case tKind t of
      KNum | expect    -> go ts (sNum (tVal t) : out) ops False
           | otherwise -> error "unexpected number"
      KVar | expect    -> go ts (sVar (round (tVal t)) : out) ops False
           | otherwise -> error "unexpected variable"
      KLParen -> go ts out (OpE KLParen 0 False False True : ops) True
      KRParen -> let (out1, ops1) = popUntilLP out ops in go ts out1 ops1 False
      KEnd | expect    -> error "unexpected end of input"
           | otherwise -> (out, ops)
      k | k `elem` [KPlus, KMinus, KStar, KSlash, KCaret] ->
            if expect
              then if k == KPlus || k == KMinus
                     then go ts out (OpE k 3 True True False : ops) True
                     else error "unexpected operator"
              else let prec = binPrec k; ra = k == KCaret
                       (out1, ops1) = popGE out ops prec ra
                   in go ts out1 (OpE k prec ra False False : ops1) True
        | otherwise -> error "unexpected token"
    go [] out ops _ = (out, ops)

    finish (out, ops) =
      let out1 = foldl' (flip applyE) out ops
      in case out1 of
           [r] -> r
           _   -> error "invalid expression"

-- ---- driver: divide & conquer (multipass family) ---------------------------
data Cand = Cand { cPos :: !Int, cPrec :: !Int, cRA :: !Bool, cUnary :: !Bool }

-- Total order used to pick the split root: lower precedence wins; on a tie,
-- right-assoc prefers the leftmost operator, left-assoc the rightmost.
better :: Cand -> Cand -> Bool
better a b
  | cPrec a /= cPrec b = cPrec a < cPrec b
  | cRA a              = cPos a < cPos b
  | otherwise          = cPos a > cPos b

floorLog2 :: Int -> Int
floorLog2 x = finiteBitSize x - 1 - countLeadingZeros x

-- Per depth, the candidate *indices* of each precedence class, ascending:
-- prec-1 (+ -), prec-2 (* /), caret, unary. Lets a long range answer
-- "rightmost prec-1/-2 in [b, e)" and "is [b, e) single-class?" by binary
-- search instead of an O(k) rescan — the fallback that caps the two former
-- Theta(n^2) worst cases (powchain / towerchain in app/Adversarial.hs) at
-- O(n log n). Thanks to laziness the arrays only materialise if a scan ever
-- exceeds its budget.
data Bucks = Bucks (UArray Int Int) (UArray Int Int) (UArray Int Int) (UArray Int Int)

-- Linear scans give up after this many candidates and consult the buckets.
mpScanBudget :: Int
mpScanBudget = 16

buildBuckets :: Array Int (Array Int Cand) -> Int -> Array Int Bucks
buildBuckets candArr dCount =
  listArray (0, max 0 (dCount - 1)) [ one (candArr ! d) | d <- [0 .. dCount - 1] ]
  where
    one a =
      let idxs  = [0 .. rangeSize (bounds a) - 1]
          sel p = let xs = [ i | i <- idxs, p (a ! i) ] in listArray (0, length xs - 1) xs
      in Bucks (sel (\c -> not (cUnary c) && cPrec c == 1))
               (sel (\c -> not (cUnary c) && cPrec c == 2))
               (sel (\c -> not (cUnary c) && cPrec c == 4))
               (sel cUnary)

-- first index into the ascending array whose value is >= key
lbVal :: UArray Int Int -> Int -> Int
lbVal a key = go 0 (rangeSize (bounds a))
  where go l r | l >= r = l
               | a ! m < key = go (m + 1) r
               | otherwise = go l m
          where m = (l + r) `div` 2

rightmostIn :: UArray Int Int -> Int -> Int -> Int   -- value in [b, e), or -1
rightmostIn a b e = let j = lbVal a e - 1
                    in if j >= 0 && a ! j >= b then a ! j else -1

anyIn :: UArray Int Int -> Int -> Int -> Bool
anyIn a b e = let j = lbVal a b in j < rangeSize (bounds a) && a ! j < e

mpRun :: Sym r => Bool -> [Tok] -> r
mpRun useSparse toks = parseRange 0 hi0 0 a0 b0 e0
  where
    n        = length toks
    arr      = listArray (0, n - 1) toks :: Array Int Tok
    hi0      = n - 1                              -- exclude the End sentinel

    (parenMatch, candArr, dCount) = buildCandidates arr n
    sparse   = if useSparse then Just (buildSparse candArr dCount) else Nothing
    bucks    = buildBuckets candArr dCount

    emptyA   = listArray (0, -1) [] :: Array Int Cand
    candRangeAt depth lo hi
      | depth < 0 || depth >= dCount = (emptyA, 0, 0)
      | otherwise = let a = candArr ! depth in (a, lb a lo, lb a hi)
    (a0, b0, e0) = candRangeAt 0 0 hi0

    -- first index whose candidate position is >= key (rangeSize if none)
    lb a key = go 0 (rangeSize (bounds a))
      where go l r | l >= r = l
                   | cPos (a ! m) < key = go (m + 1) r
                   | otherwise = go l m
              where m = (l + r) `div` 2

    stQuery depth lo hi
      | lo >= hi  = -1
      | otherwise =
          let a      = candArr ! depth
              levels = case sparse of Just s -> s ! depth; Nothing -> []
              k      = floorLog2 (hi - lo)
              x      = (levels !! k) ! lo
              y      = (levels !! k) ! (hi - 2 ^ k)
          in if better (a ! x) (a ! y) then x else y

    parseRange :: Sym r => Int -> Int -> Int -> Array Int Cand -> Int -> Int -> r
    parseRange lo hi depth a b e
      | lo >= hi = error "empty sub-expression"
      | b < e && flat = if not chainRA then leftFold else rightFold
      | s /= -1 =
          let c = a ! s; pos = cPos c
          in if cUnary c
               then (if tKind (arr ! pos) == KMinus then sNeg else sPos)
                      (parseRange (pos + 1) hi depth a (s + 1) e)
               else sBin (opOf (tKind (arr ! pos)))
                      (parseRange lo pos depth a b s)
                      (parseRange (pos + 1) hi depth a (s + 1) e)
      | tKind (arr ! lo) == KLParen && parenMatch ! lo == hi - 1 =
          let d = depth + 1
              (a2, b2, e2) = candRangeAt d (lo + 1) (hi - 1)
          in parseRange (lo + 1) (hi - 1) d a2 b2 e2
      | hi - lo == 1 = case tKind (arr ! lo) of
          KNum -> sNum (tVal (arr ! lo))
          KVar -> sVar (round (tVal (arr ! lo)))
          _    -> error "syntax error"
      | otherwise = error ("syntax error at position " ++ show (tPos (arr ! lo)))
      where
        chainPrec = cPrec (a ! b)
        chainRA   = cRA (a ! b)
        -- hybrid flatness check: a mixed range is usually disproved within
        -- the scanned prefix; only a uniform prefix longer than the budget
        -- asks the buckets to vouch for the rest (never an O(k) rescan)
        uniform i j = all (\x -> not (cUnary (a ! x)) && cPrec (a ! x) == chainPrec) [i .. j]
        flat
          | e - b <= mpScanBudget = b < e && uniform b (e - 1)
          | uniform b (b + mpScanBudget - 1) = flatByBuckets depth b e
          | otherwise = False
        s = splitAt' a b e lo depth

        leftFold =
          let acc0 = parseRange lo (cPos (a ! b)) depth a e e
              step acc i =
                let pos = cPos (a ! i)
                    nhi = if i + 1 < e then cPos (a ! (i + 1)) else hi
                    rhs = parseRange (pos + 1) nhi depth a e e
                in sBin (opOf (tKind (arr ! pos))) acc rhs
          in foldl' step acc0 [b .. e - 1]

        rightFold =
          let idxs   = [b .. e - 1]
              starts = lo : map (\i -> cPos (a ! i) + 1) idxs
              ends   = map (\i -> cPos (a ! i)) idxs ++ [hi]
              parts  = zipWith (\st en -> parseRange st en depth a e e) starts ends
          in foldr (\(i, p) acc -> sBin (opOf (tKind (arr ! cPos (a ! i)))) p acc)
                   (last parts) (zip idxs (init parts))

    -- True iff every candidate in [b, e) is binary with one precedence,
    -- answered from the buckets in O(log k).
    flatByBuckets depth b e =
      let Bucks p1 p2 pc pu = bucks ! depth
      in not (anyIn pu b e) &&
         length (filter id [anyIn p1 b e, anyIn p2 b e, anyIn pc b e]) == 1

    -- Bucket equivalent of the full RTL scan: rightmost prec-1 candidate in
    -- [b, e), else rightmost prec-2, else the range's first candidate — a
    -- leading unary (lowest precedence present) or the leftmost right-assoc
    -- ^. A unary not at the range start cannot be first: the token before it
    -- is a same-depth operator, itself the earlier candidate.
    splitByBuckets depth b e =
      let Bucks p1 p2 _ _ = bucks ! depth
      in case rightmostIn p1 b e of
           i | i >= 0 -> i
           _ -> case rightmostIn p2 b e of
                  i | i >= 0 -> i
                  _          -> b

    -- split index, threading depth for the sparse path
    splitAt' a b e lo depth
      | Just _ <- sparse =
          let si = stQuery depth b e
          in if si < 0 then -1
             else let c = a ! si
                  in if cUnary c && cPos c /= lo
                       then head ([ i | i <- [b .. e - 1]
                                      , not (cUnary (a ! i)) || cPos (a ! i) == lo ] ++ [-1])
                       else si
      | otherwise = linear (e - 1) (-1) mpScanBudget
      where
        -- bounded RTL scan: past the budget without a prec-1 hit, the answer
        -- comes from the buckets instead (O(log k), never O(k))
        linear i best fuel
          | i < b = best
          | fuel <= 0 = splitByBuckets depth b e
          | cUnary c && cPos c /= lo = linear (i - 1) best (fuel - 1)
          | best == (-1) || cPrec c < cPrec (a ! best) =
              if cPrec c == 1 && not (cRA c) then i else linear (i - 1) i (fuel - 1)
          | cPrec c == cPrec (a ! best) && cRA (a ! best) = linear (i - 1) i (fuel - 1)
          | otherwise = linear (i - 1) best (fuel - 1)
          where c = a ! i

-- one O(n) scan: candidate lists per paren-depth + matching-paren array
buildCandidates :: Array Int Tok -> Int -> (Array Int Int, Array Int (Array Int Cand), Int)
buildCandidates arr n =
  let (_, _, _, cmap, matches) = foldl' step (0, True, [], IM.empty, []) [0 .. n - 2]
      dCount = if IM.null cmap then 0 else fst (IM.findMax cmap) + 1
      candArr = listArray (0, max 0 (dCount - 1))
                  [ let cs = reverse (IM.findWithDefault [] d cmap)
                    in listArray (0, length cs - 1) cs
                  | d <- [0 .. dCount - 1] ]
      pm = accumArray (\_ v -> v) 0 (0, n - 1) matches :: Array Int Int
  in (pm, candArr, dCount)
  where
    step (depth, expect, stack, cmap, matches) i = case tKind (arr ! i) of
      KLParen -> (depth + 1, True, i : stack, cmap, matches)
      KRParen -> case stack of
                   (o : st') -> (depth - 1, False, st', cmap, (o, i) : (i, o) : matches)
                   []        -> (depth - 1, False, [], cmap, matches)
      KNum    -> (depth, False, stack, cmap, matches)
      KVar    -> (depth, False, stack, cmap, matches)
      k | k `elem` [KPlus, KMinus, KStar, KSlash, KCaret] ->
            let cand | expect = if k == KPlus || k == KMinus
                                  then Cand i 3 False True
                                  else error "unexpected operator"
                     | otherwise = Cand i (binPrec k) (k == KCaret) False
            in (depth, True, stack, IM.insertWith (++) depth [cand] cmap, matches)
        | otherwise -> (depth, expect, stack, cmap, matches)

-- per-depth sparse table: levels !! k ! i = argmin (better) over [i, i+2^k)
buildSparse :: Array Int (Array Int Cand) -> Int -> Array Int [Array Int Int]
buildSparse candArr dCount =
  listArray (0, max 0 (dCount - 1))
    [ buildOne (candArr ! d) | d <- [0 .. dCount - 1] ]
  where
    buildOne a =
      let sz = rangeSize (bounds a)
      in if sz <= 0 then []
         else let logn = floorLog2 sz + 1
                  lvl0 = listArray (0, sz - 1) [0 .. sz - 1]
                  build prev k =
                    let half = 2 ^ (k - 1)
                    in listArray (0, sz - 1)
                         [ if i + 2 ^ k <= sz
                             then let x = prev ! i; y = prev ! (i + half)
                                  in if better (a ! x) (a ! y) then x else y
                             else prev ! i
                         | i <- [0 .. sz - 1] ]
                  levels = lvl0 : [ build (levels !! (k - 1)) k | k <- [1 .. logn - 1] ]
              in levels

-- ---- driver: REVERSE multipass (bottom-up, innermost/highest first) --------
-- Dual of mpRun: reduce the tightest-binding things first (deepest parens, then
-- ^, then * /, then + -), agglomerating outward. "Multipass" in its original
-- sense: one reduction pass per precedence level. Same tree, opposite order.
data RItem r = ROpd r | RUn Kind | ROp Kind

isBarrier :: Kind -> Bool
isBarrier k = k == KPlus || k == KMinus || k == KStar || k == KSlash

reverseMpParse :: Sym r => [Tok] -> r
reverseMpParse toks = reduceRange 0 (n - 1)
  where
    n   = length toks
    arr = listArray (0, n - 1) toks :: Array Int Tok
    pm  = parenMatchArr arr n

    reduceRange lo hi
      | lo >= hi  = error "empty sub-expression"
      | otherwise =
          let raw  = build lo True
              flat = splitSegments raw
          in case reduceBinLevel [KPlus, KMinus]
                    (reduceBinLevel [KStar, KSlash] flat) of
               [ROpd r] -> r
               _        -> error "reduction failed"
      where
        -- materialise items, recursing into parens (innermost first)
        build i expect
          | i >= hi   = if expect then error "unexpected end of sub-expression" else []
          | otherwise = case tKind (arr ! i) of
              KLParen -> let j = pm ! i in ROpd (reduceRange (i + 1) j) : build (j + 1) False
              KNum    -> ROpd (sNum (tVal (arr ! i)))           : build (i + 1) False
              KVar    -> ROpd (sVar (round (tVal (arr ! i))))   : build (i + 1) False
              k | k `elem` [KPlus, KMinus, KStar, KSlash, KCaret] ->
                    if expect
                      then if k == KPlus || k == KMinus
                             then RUn k : build (i + 1) True
                             else error "unexpected operator"
                      else ROp k : build (i + 1) True
                | otherwise -> error "syntax error"

    -- split by * / + - barriers; reduce each ^/unary segment to one operand
    -- (segments accumulate reversed — exactly what reduceSegRev consumes)
    splitSegments items = go items []
      where
        go [] segRev = [ROpd (reduceSegRev segRev)]
        go (it : rest) segRev = case it of
          ROp k | isBarrier k -> ROpd (reduceSegRev segRev) : it : go rest []
          _                   -> go rest (it : segRev)

-- ---- multipass-reverse, fused ("fold") -------------------------------------
-- Same bottom-up order as reverseMpParse, but the two binary levels are
-- contracted in the same sweep that materialises the items: after a ^/unary
-- segment folds on a barrier only two left-associative levels remain, which
-- need one pending (op, accumulator) each — @tm@ for * / and @sm@ for + — not
-- a list. A '(' pushes (segment, tm, sm) on a frame stack; there is no
-- recursion, no paren-matching prepass and no per-level re-scan. The scan
-- alternates an operand mode and an operator mode, like the C++ version.
-- The segment is kept most-recent-first, so a left fold over it IS the
-- right-to-left fold (right-assoc ^; ^ binds tighter than a prefix sign).
data FItem r = FUn !Kind | FPow r

reverseFoldParse :: Sym r => [Tok] -> r
reverseFoldParse = operand [] [] Nothing Nothing
  where
    -- operand mode: number, variable, '(' or a prefix sign
    operand _ _ _ _ [] = error "unexpected end of input"
    operand seg frames tm sm (t : ts) = case tKind t of
      KNum    -> operator (sNum (tVal t)) seg frames tm sm ts
      KVar    -> operator (sVar (round (tVal t))) seg frames tm sm ts
      KLParen -> operand [] ((seg, tm, sm) : frames) Nothing Nothing ts
      k | k == KPlus || k == KMinus -> operand (FUn k : seg) frames tm sm ts
      _ -> error ("expected operand at position " ++ show (tPos t))

    -- operator mode: binary op, ')' or end; ')' stays in this mode
    operator _ _ _ _ _ [] = error "unexpected end of input"
    operator cur seg frames tm sm (t : ts) = case tKind t of
      k | k == KPlus || k == KMinus ->
            let !v  = foldSeg seg cur
                !tv = closeMul tm v
                !sv = closeAdd sm tv
            in operand [] frames Nothing (Just (opOf k, sv)) ts
        | k == KStar || k == KSlash ->
            let !v  = foldSeg seg cur
                !tv = closeMul tm v
            in operand [] frames (Just (opOf k, tv)) sm ts
      KCaret  -> operand (FPow cur : seg) frames tm sm ts
      KRParen -> case frames of
        []                       -> error ("mismatched parenthesis at position " ++ show (tPos t))
        ((seg0, tm0, sm0) : fs)  -> operator (closeRange cur seg tm sm) seg0 fs tm0 sm0 ts
      KEnd | null frames -> closeRange cur seg tm sm
           | otherwise   -> error "missing ')'"
      _ -> error ("expected operator at position " ++ show (tPos t))

    foldSeg seg cur = foldl' step cur seg
      where step acc (FUn k)  = if k == KMinus then sNeg acc else sPos acc
            step acc (FPow b) = sBin Pow b acc
    closeMul Nothing        v = v
    closeMul (Just (op, l)) v = sBin op l v
    closeAdd Nothing        v = v
    closeAdd (Just (op, l)) v = sBin op l v
    closeRange cur seg tm sm = closeAdd sm (closeMul tm (foldSeg seg cur))

-- Reduce one REVERSED ^/unary segment — (un* opd (^ un* opd)*) read backwards —
-- by folding right-to-left: right-assoc ^ and the ^-binds-tighter-than-unary
-- corner (-2^2 = -4, 2^-3 = 0.125) fall out naturally walking from the right.
reduceSegRev :: Sym r => [RItem r] -> r
reduceSegRev (ROpd r0 : rest0) = go r0 rest0
  where
    go acc []                            = acc
    go acc (RUn k : more)                = go ((if k == KMinus then sNeg else sPos) acc) more
    go acc (ROp KCaret : ROpd b : more)  = go (sBin Pow b acc) more
    go _ _                               = error "malformed segment"
reduceSegRev _ = error "malformed segment"

-- left-to-right left-associative contraction of one binary level
reduceBinLevel :: Sym r => [Kind] -> [RItem r] -> [RItem r]
reduceBinLevel ops (ROpd x0 : rest) = collapse x0 rest
  where
    collapse acc (ROp k : ROpd y : more)
      | k `elem` ops = collapse (sBin (opOf k) acc y) more
      | otherwise    = ROpd acc : ROp k : collapse y more
    collapse acc [] = [ROpd acc]
    collapse _ _    = error "malformed binary level"
reduceBinLevel _ _ = error "expected operand"

parenMatchArr :: Array Int Tok -> Int -> Array Int Int
parenMatchArr arr n = accumArray (\_ v -> v) 0 (0, n - 1) matches
  where
    (_, matches) = foldl' step ([], []) [0 .. n - 2]
    step (stack, ms) i = case tKind (arr ! i) of
      KLParen -> (i : stack, ms)
      KRParen -> case stack of
                   (o : s') -> (s', (o, i) : (i, o) : ms)
                   []       -> ([], ms)
      _ -> (stack, ms)

-- ---- bytecode VM -----------------------------------------------------------
data Instr = IPush !Double | ILoad !Int | INeg | IBin !Op

-- compile (shunting-yard) to a reusable instruction list
bcCompile :: [Tok] -> [Instr]
bcCompile toks = compile toks [] [] True
  where
    applyE (OpE k _ _ unary lp) code
      | lp = error "mismatched parenthesis"
      | unary = if k == KMinus then INeg : code else code
      | otherwise = IBin (opOf k) : code

    popUntilLP code (o : ops)
      | not (isLP o) = popUntilLP (applyE o code) ops
      | otherwise    = (code, ops)
    popUntilLP _ [] = error "mismatched parenthesis"

    popGE code (o : ops) prec ra
      | not (isLP o) && (oePrec o > prec || (oePrec o == prec && not ra))
        = popGE (applyE o code) ops prec ra
    popGE code ops _ _ = (code, ops)

    compile (t : ts) code ops expect = case tKind t of
      KNum | expect    -> compile ts (IPush (tVal t) : code) ops False
           | otherwise -> error "unexpected number"
      KVar | expect    -> compile ts (ILoad (round (tVal t)) : code) ops False
           | otherwise -> error "unexpected variable"
      KLParen -> compile ts code (OpE KLParen 0 False False True : ops) True
      KRParen -> let (c1, ops1) = popUntilLP code ops in compile ts c1 ops1 False
      KEnd | expect    -> error "unexpected end of input"
           | otherwise -> reverse (foldl' (flip applyE) code ops)
      k | k `elem` [KPlus, KMinus, KStar, KSlash, KCaret] ->
            if expect
              then if k == KPlus || k == KMinus
                     then compile ts code (OpE k 3 True True False : ops) True
                     else error "unexpected operator"
              else let prec = binPrec k; ra = k == KCaret
                       (c1, ops1) = popGE code ops prec ra
                   in compile ts c1 (OpE k prec ra False False : ops1) True
        | otherwise -> error "unexpected token"
    compile [] code ops _ = reverse (foldl' (flip applyE) code ops)

bcRun :: Env -> [Instr] -> Double
bcRun env = go `flip` []
  where
    go [] [v] = v
    go (IPush x : cs) st = go cs (x : st)
    go (ILoad i : cs) st = go cs (env i : st)
    go (INeg : cs) (x : st) = go cs (negate x : st)
    go (IBin op : cs) (r : l : st) = go cs (applyOp op l r : st)
    go _ _ = error "invalid expression"

bcEval :: Env -> [Tok] -> Double
bcEval env toks = bcRun env (bcCompile toks)

-- ---- reeval API: compile once -> reusable (Env -> Double) ------------------
-- Build the compiled form once (captured in the returned closure); evaluating
-- with a fresh Env only re-walks it. `reparse-rd` re-parses on every call.
arenaClosure :: Arena -> (Env -> Double)
arenaClosure (Arena f) =
  let (root, (cnt, xs)) = f (0, [])
      arr = listArray (0, cnt - 1) (reverse xs) :: Array Int Node
      go env i = case arr ! i of
        NNum x      -> x
        NVar v      -> env v
        NNeg c      -> negate (go env c)
        NBin op l r -> applyOp op (go env l) (go env r)
  in if cnt == 0 then error "empty expression" else \env -> go env root

reevalForms :: [(String, String -> (Env -> Double))]
reevalForms =
  [ ("ast-ptr",         \src -> let e = rdParse (tokenize src) :: Expr in \env -> evalExpr env e)
  , ("ast-arena",       \src -> arenaClosure (rdParse (tokenize src) :: Arena))
  , ("multipass-arena", \src -> arenaClosure (mpRun False (tokenize src) :: Arena))
  , ("bytecode",        \src -> let c = bcCompile (tokenize src) in \env -> bcRun env c)
  , ("reparse-rd",      \src env -> evalExpr env (rdParse (tokenize src)))
  ]

-- ---- registry --------------------------------------------------------------
data Evaluator = Evaluator { evName :: String, evRun :: Env -> String -> Double }

mkAst :: String -> (forall r. Sym r => [Tok] -> r) -> Evaluator
mkAst name parse = Evaluator name (\env src -> evalExpr env (parse (tokenize src)))

mkArena :: String -> (forall r. Sym r => [Tok] -> r) -> Evaluator
mkArena name parse = Evaluator name (\env src -> evalArena env (parse (tokenize src)))

mkDirect :: String -> (forall r. Sym r => [Tok] -> r) -> Evaluator
mkDirect name parse = Evaluator name (\env src -> runDirect (parse (tokenize src)) env)

allEvaluators :: [Evaluator]
allEvaluators =
  [ mkAst    "ast-recursive-descent"    rdParse
  , mkAst    "ast-shunting-yard"        syParse
  , mkAst    "ast-pratt"                prattParse
  , mkArena  "ast-arena"                rdParse
  , mkAst    "multipass"                (mpRun False)
  , mkArena  "multipass-arena"          (mpRun False)
  , mkDirect "direct-mp"                (mpRun False)
  , mkArena  "multipass-bfs"            (mpRun True)
  , mkArena  "multipass-reverse"        reverseMpParse
  , mkArena  "multipass-reverse-fold"   reverseFoldParse
  , mkDirect "direct-recursive-descent" rdParse
  , mkDirect "direct-shunting-yard"     syParse
  , mkDirect "direct-reverse"           reverseFoldParse
  , Evaluator "bytecode-vm"             (\env src -> bcEval env (tokenize src))
  ]
