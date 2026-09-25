-- | Benchmark the fifteen strategies on the shared corpora (bench/corpus/).
-- Cross-checks all strategies agree, then prints ns/leaf per strategy.
-- Uses only boot libraries (Data.Time) — no external dependencies.
module Main (main) where

import Control.Exception (evaluate)
import Control.Monad (forM_, forM)
import Data.List (foldl', transpose)
import Data.Time.Clock (getCurrentTime, diffUTCTime)
import Text.Printf (printf)

import MathParser.Strategies

sizes :: [Int]
sizes = [10, 100, 1000, 10000]

-- best-of-N with N >= 3 everywhere, so no published number rests on a
-- single unrepeated measurement
reps :: Int -> Int
reps n = if n <= 1000 then 5 else 3

corpusPath :: Int -> String
corpusPath n = "../bench/corpus/n" ++ show n ++ ".txt"

constEnv :: Env
constEnv = const 0   -- corpora are numeric; variables never appear

same :: Double -> Double -> Bool
same a b
  | isNaN a && isNaN b = True
  | a == b = True
  | isInfinite a || isInfinite b = False
  | otherwise = abs (a - b) <= 1e-6 * maximum [1, abs a, abs b]

main :: IO ()
main = do
  corpora <- forM sizes $ \n -> do
    txt <- readFile (corpusPath n)
    pure (n, filter (not . null) (lines txt))

  putStrLn "== Haskell: math-expression evaluator comparison ==\n"

  let evs = allEvaluators
      ref = head evs
  bad <- fmap sum $ forM corpora $ \(_, corpus) -> do
    let subset = take 500 corpus
    fmap sum $ forM subset $ \expr -> do
      let r0 = evRun ref constEnv expr
      pure $ length [ () | ev <- tail evs, not (same r0 (evRun ev constEnv expr)) ]
  printf "Correctness: %s\n\n"
         (if bad == (0 :: Int) then "all strategies agree" else show bad ++ " MISMATCHES")

  printf "%-26s%12s%12s%12s%12s\n" "strategy" "n=10" "n=100" "n=1000" "n=10000"
  putStrLn (replicate (26 + 48) '-')
  -- Interleaved: each rep times every strategy once, round-robin, so slow
  -- drift on the runner lands on all strategies alike instead of on whichever
  -- ran late. Best-of-reps per cell. The heap now carries over from one
  -- strategy to the next within a rep; each timing forces its own result.
  cells <- forM corpora $ \(n, corpus) -> do
    rounds <- forM [1 .. reps n] $ \r ->
      forM evs $ \ev -> timeNs ev corpus r
    let perLeaf t = t / fromIntegral (length corpus) / fromIntegral n
    pure (map (perLeaf . minimum) (transpose rounds))
  forM_ (zip evs (transpose cells)) $ \(ev, row) -> do
    printf "%-26s" (evName ev)
    forM_ row $ \ns -> printf "%12.1f" ns
    putStrLn ""

-- one timed pass over the corpus, in ns. Seed the fold with the rep index so
-- GHC -O2 can't share the result as a CAF across reps (which would make every
-- rep but the first measure 0).
timeNs :: Evaluator -> [String] -> Int -> IO Double
timeNs ev corpus r = do
  t0 <- getCurrentTime
  s  <- evaluate (foldl' (\acc e -> acc + evRun ev constEnv e) (fromIntegral r) corpus)
  t1 <- s `seq` getCurrentTime
  pure (realToFrac (diffUTCTime t1 t0) * 1e9)   -- seconds -> nanoseconds
