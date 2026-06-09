{-# LANGUAGE BangPatterns #-}

-- | Batch-throughput scaling for Haskell: split the shared corpus across W
-- forkIO workers (with `setNumCapabilities`) and see how throughput scales at
-- W = 1, 2, 4. Build is -threaded so capabilities map to OS threads. Unlike
-- Python there is no GIL — pure evaluation scales with cores.
module Main (main) where

import Control.Concurrent (forkIO)
import Control.Concurrent.MVar (newEmptyMVar, putMVar, takeMVar)
import Control.Exception (evaluate)
import Control.Monad (forM, forM_)
import Data.List (foldl')
import Data.Time.Clock (getCurrentTime, diffUTCTime)
import GHC.Conc (setNumCapabilities)
import Text.Printf (printf)

import MathParser.Strategies

constEnv :: Env
constEnv = const 0

workers :: [Int]
workers = [1, 2, 4]

sizeN :: Int
sizeN = 100

strategies :: [String]
strategies =
  [ "ast-recursive-descent", "ast-arena", "multipass-reverse"
  , "direct-recursive-descent", "bytecode-vm" ]

splitN :: Int -> [a] -> [[a]]
splitN n xs = go xs
  where
    sz = (length xs + n - 1) `div` n
    go [] = []
    go ys = let (a, b) = splitAt sz ys in a : go b

-- run W workers over the chunks, force each partial sum, return the total
parRun :: Evaluator -> [[String]] -> IO Double
parRun ev parts = do
  mvars <- forM parts $ \chunk -> do
    mv <- newEmptyMVar
    _  <- forkIO $ do
            let !s = foldl' (\a e -> a + evRun ev constEnv e) 0 chunk
            s' <- evaluate s
            putMVar mv s'
    pure mv
  rs <- mapM takeMVar mvars
  evaluate (sum rs)

bestTime :: Int -> IO a -> IO Double
bestTime reps act = do
  ts <- forM [1 .. reps] $ \_ -> do
    t0 <- getCurrentTime
    _  <- act
    t1 <- getCurrentTime
    pure (realToFrac (diffUTCTime t1 t0) :: Double)
  pure (minimum ts)

main :: IO ()
main = do
  txt <- readFile ("../bench/corpus/n" ++ show sizeN ++ ".txt")
  let corpus = filter (not . null) (lines txt)
      work   = concat (replicate 4 corpus)
      leaves = fromIntegral (length work * sizeN) :: Double

  putStrLn "== Haskell: batch-throughput scaling (-threaded) =="
  printf "corpus=n%d x4 (%d exprs, throughput Mleaf/s — higher is better)\n\n"
         sizeN (length work)
  printf "%-26s%10s%10s%10s%12s\n" "strategy" "W=1" "W=2" "W=4" "speedup@4"
  putStrLn (replicate (26 + 42) '-')

  forM_ strategies $ \name -> do
    let ev = head [e | e <- allEvaluators, evName e == name]
    tputs <- forM workers $ \w -> do
      setNumCapabilities w
      let parts = splitN w work
      t <- bestTime 2 (parRun ev parts)
      pure (leaves / t / 1e6)
    printf "%-26s%10.2f%10.2f%10.2f%11.2fx\n"
           name (tputs !! 0) (tputs !! 1) (tputs !! 2)
           (last tputs / head tputs)
