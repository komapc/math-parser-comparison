{-# LANGUAGE BangPatterns #-}

-- | Re-evaluation benchmark: compile once, evaluate many against changing
-- variables. Mirrors C++/Python. The compiled forms (pointer AST, arena AST,
-- bytecode) build a reusable structure once; `reparse-rd` re-parses every call.
--
-- Laziness handling: compiled forms are built once and forced by a warmup eval,
-- then steady-state evals are timed; `reparse-rd`'s closure is re-created inside
-- the timed loop so the parse genuinely re-runs. The rep index is threaded into
-- the fold seed so GHC can't share a whole pass across reps.
module Main (main) where

import Control.Exception (evaluate)
import Control.Monad (forM, forM_)
import Data.Array (listArray, (!))
import Data.List (foldl')
import Data.Time.Clock (getCurrentTime, diffUTCTime)
import Text.Printf (printf)

import MathParser.Strategies (Env, reevalForms)

sizes :: [Int]
sizes = [10, 100, 1000]

reps :: Int
reps = 5

-- 8 deterministic variable environments, values in [0.5, 2.0]
envs :: [Env]
envs = [ mkEnv [ 0.5 + 1.5 * fromIntegral ((e * 31 + i * 17 + 7) `mod` 100) / 100
               | i <- [0 .. 25] ]
       | e <- [0 .. 7 :: Int] ]
  where mkEnv xs = let a = listArray (0, length xs - 1) xs in (a !)

envOf :: Int -> Env
envOf r = envs !! (r `mod` length envs)

-- best (minimum) ns over `reps` runs; the action takes the rep index
bestNs :: Int -> (Int -> IO a) -> IO Double
bestNs n act = do
  ts <- forM [1 .. n] $ \r -> do
    t0 <- getCurrentTime
    _  <- act r
    t1 <- getCurrentTime
    pure (realToFrac (diffUTCTime t1 t0) * 1e9 :: Double)
  pure (minimum ts)

timeOnce :: IO a -> IO Double
timeOnce act = do
  t0 <- getCurrentTime
  _  <- act
  t1 <- getCurrentTime
  pure (realToFrac (diffUTCTime t1 t0) * 1e9 :: Double)

main :: IO ()
main = do
  putStrLn "== Haskell: re-evaluation (compile once, evaluate many) ==\n"
  putStrLn "Variables a-d, values in [0.5, 2.0]"
  printf "%-16s%8s%8s%14s%14s%12s\n"
         "strategy" "leaves" "exprs" "compile ns" "per-eval ns" "break-even"
  putStrLn (replicate 72 '-')

  forM_ sizes $ \size -> do
    txt <- readFile ("../bench/corpus/vars_n" ++ show size ++ ".txt")
    let corpus = filter (not . null) (lines txt)
        n      = fromIntegral (length corpus) :: Double

    rows <- forM reevalForms $ \(name, comp) ->
      if name == "reparse-rd"
        then do
          -- re-create the closure inside the fold => genuinely re-parses
          ev <- bestNs reps $ \r ->
            evaluate (foldl' (\a src -> a + comp src (envOf r)) (fromIntegral r) corpus)
          pure (name, 0.0, ev / n)
        else do
          let forms = map comp corpus
          -- warmup forces each compiled structure (build + one walk)
          warm <- timeOnce (evaluate (foldl' (\a f -> a + f (head envs)) 0 forms))
          ev <- bestNs reps $ \r ->
            evaluate (foldl' (\a f -> a + f (envOf r)) (fromIntegral r) forms)
          pure (name, max 0 (warm - ev) / n, ev / n)

    let reparseEval = head [ e | (nm, _, e) <- rows, nm == "reparse-rd" ]
    forM_ rows $ \(name, compileNs, evalNs) -> do
      let denom = reparseEval - evalNs
          be | name == "reparse-rd" = "(baseline)"
             | denom <= 0           = "n/a"
             | otherwise            = printf "%.1f" (compileNs / denom)
      printf "%-16s%8d%8d%14.1f%14.2f%12s\n"
             name size (length corpus) compileNs evalNs (be :: String)
    putStrLn ""
