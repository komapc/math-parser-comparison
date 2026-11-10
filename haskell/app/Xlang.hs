-- Cross-language check, Haskell side: evaluate every line of the given files
-- with the first registered strategy and print one result per line — `E`
-- (rejected), `nan`, or the 16-hex-digit IEEE-754 bits of the value.
-- bench/xlang_compare.py compares this with the other three languages'.
-- Within a language every strategy shares one lexer, so the in-language fuzz
-- cannot see a lexer bug; only this comparison can.
module Main (main) where

import Control.Exception (SomeException, evaluate, try)
import GHC.Float (castDoubleToWord64)
import System.Environment (getArgs)
import Text.Printf (printf)

import MathParser.Strategies

env :: Env
env i = case i of 0 -> 2; 1 -> 3; 2 -> 4; 3 -> 5; _ -> 0

main :: IO ()
main = do
  paths <- getArgs
  let ev = head allEvaluators
  mapM_ (\p -> readFile p >>= mapM_ (one ev) . lines) paths
  where
    one ev e = do
      r <- try (evaluate (evRun ev env e)) :: IO (Either SomeException Double)
      case r of
        Left _ -> putStrLn "E"
        Right v
          | isNaN v -> putStrLn "nan"
          | otherwise -> printf "%016x\n" (castDoubleToWord64 v)
