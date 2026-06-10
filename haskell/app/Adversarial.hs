{-# LANGUAGE BangPatterns #-}

-- | Adversarial (structured, non-random) inputs that separate the multipass
-- family. Two flat chains, no parentheses (mirrors
-- cpp/bench/adversarial_bench.cpp):
--
--   powchain  3^2 * 2^2 / 2^2 * 3^3 / 3^3 ...   alternating-precedence (^ vs */)
--   sumchain  1 + 2 - 3 + 4 ...                 single-precedence (control)
--
-- On powchain the top-down splitters degenerate: the candidate list mixes
-- prec 2 and prec 4, so the flat-chain fold never applies and the linear
-- right-to-left split scan has no prec-1 early exit — every split rescans its
-- whole range: Theta(n^2) (multipass, multipass-arena, direct-mp). The
-- sparse-table RMQ (multipass-bfs) answers splits in O(1) — the one input
-- family where its precompute pays off. Bottom-up multipass-reverse reduces
-- each level in one pass regardless: Theta(n). sumchain is the control where
-- the whole family stays linear.
module Main (main) where

import Control.Monad (forM, forM_)
import Data.Time.Clock (getCurrentTime, diffUTCTime)
import Text.Printf (printf)

import MathParser.Strategies

sizes :: [Int]
sizes = [256, 1024, 4096]

reps :: Int
reps = 3

constEnv :: Env
constEnv = const 0

-- m factors b^e; ops alternate * and /; adjacent factor pairs are identical,
-- so the value stays exactly 9.0 and the cross-check is an equality test.
powChain :: Int -> String
powChain m = concat $ "3 ^ 2" :
  [ op ++ show (2 + k `mod` 8) ++ " ^ " ++ show (2 + k `mod` 3)
  | i <- [1 .. m - 1]
  , let k  = (i - 1) `div` 2
        op = if odd i then " * " else " / " ]

-- m terms; ops alternate + and -; value stays small.
sumChain :: Int -> String
sumChain m = concat $ "1" :
  [ op ++ show (1 + i `mod` 9)
  | i <- [1 .. m - 1]
  , let op = if odd i then " + " else " - " ]

bestNs :: Evaluator -> String -> Int -> IO Double
bestNs ev expr salt = do
  ts <- forM [1 .. reps] $ \r -> do
    t0 <- getCurrentTime
    let !v = evRun ev (const (fromIntegral (salt * r))) expr
    t1 <- v `seq` getCurrentTime
    pure (realToFrac (diffUTCTime t1 t0) :: Double)
  pure (minimum ts * 1e9)

runShape :: String -> (Int -> String) -> Int -> IO ()
runShape title gen leavesPerFactor = do
  printf "-- %s --\n" title
  printf "%-26s%12s%12s%12s   (ns/leaf; flat = linear, ~4x/col = quadratic)\n"
         "strategy" ("m=" ++ show (sizes !! 0)) ("m=" ++ show (sizes !! 1))
         ("m=" ++ show (sizes !! 2))
  let exprs = map gen sizes
      ref   = evRun (head allEvaluators) constEnv (last exprs)
  forM_ allEvaluators $ \ev -> do
    let got = evRun ev constEnv (last exprs)
    if got /= ref
      then printf "MISMATCH [%s]: %g != %g\n" (evName ev) got ref
      else pure ()
  forM_ (zip [1 ..] allEvaluators) $ \(salt, ev) -> do
    printf "%-26s" (evName ev)
    forM_ (zip sizes exprs) $ \(m, expr) -> do
      ns <- bestNs ev expr salt
      printf "%12.0f" (ns / fromIntegral (m * leavesPerFactor))
    printf "\n"
  printf "\n"

main :: IO ()
main = do
  putStrLn "== Haskell: adversarial chains (structured inputs, no parens) ==\n"
  runShape "powchain: b^e * b^e / ... (mixed precedence)" powChain 2
  runShape "sumchain: 1 + 2 - 3 + ... (single precedence — control)" sumChain 1
