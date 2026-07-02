-- | Correctness suite — mirrors cpp/tests/test_parsers.cpp and python/test_parsers.py.
-- Variable environment a=2, b=3, c=4, d=5. Exit 1 on any failure.
module Main (main) where

import Control.Exception (SomeException, evaluate, try)
import System.Exit (exitFailure, exitSuccess)
import Text.Printf (printf)

import MathParser.Strategies

env :: Env
env i = case i of 0 -> 2; 1 -> 3; 2 -> 4; 3 -> 5; _ -> 0

cases :: [(String, Double)]
cases =
  [ ("a + b * c", 14), ("(a + b) * c", 20), ("a * (b + c) - d", 9)
  , ("(a + b) * b / d", 3), ("-(b + c) * a", -14), ("d - a - b", 0)
  , ("d * c / a", 10), ("a ^ a ^ a", 16), ("-a ^ a", -4)
  , ("a ^ -b", 0.125), ("3.5 * a + 1.5", 8.5), ("2e2 + a", 202)
  , ("--a", 2), ("-a * b", -6), ("+d - +a", 3)
  -- IEEE special values — pow/div corners shared across the languages
  , ("0 ^ -1", 1 / 0), ("1 / (0 * -1)", -1 / 0)
  , ("(0 - 1e155) ^ 3", -1 / 0), ("(0 / 0) / 0", 0 / 0)
  , ("(0 - a) ^ 0.5", 0 / 0)
  ]

errs :: [String]
errs =
  [ "a +", "(a + b", "a b", "* a", "a + * b", ""
  -- stray ')' and adjacent operand groups, and a digitless number
  -- (regression: the lexer accepted "." as 0.0)
  , "a)", "(a)(b)", "a(3)", "."
  ]

nearly :: Double -> Double -> Bool
nearly a b
  | isNaN b   = isNaN a
  | a == b    = True            -- covers ±inf
  | otherwise = abs (a - b) <= 1e-9 * maximum [1, abs a, abs b]

main :: IO ()
main = do
  results <- mapM checkOne allEvaluators
  let checks   = sum (map fst results)
      failures = sum (map snd results)
  printf "\n%d checks across %d evaluators, %d failure(s)\n"
         checks (length allEvaluators) failures
  if failures == 0 then exitSuccess else exitFailure

checkOne :: Evaluator -> IO (Int, Int)
checkOne ev = do
  vs <- mapM (checkValue ev) cases
  es <- mapM (checkError ev) errs
  pure (length vs + length es, length (filter not vs) + length (filter not es))

-- True if the case passed.
checkValue :: Evaluator -> (String, Double) -> IO Bool
checkValue ev (expr, expected) = do
  r <- try (evaluate (evRun ev env expr)) :: IO (Either SomeException Double)
  case r of
    Right got | nearly got expected -> pure True
              | otherwise -> do
                  printf "FAIL [%-26s] %-16s = %g, expected %g\n" (evName ev) (show expr) got expected
                  pure False
    Left e -> do
      printf "FAIL [%-26s] %-16s threw: %s\n" (evName ev) (show expr) (show e)
      pure False

-- True if the error case correctly raised.
checkError :: Evaluator -> String -> IO Bool
checkError ev expr = do
  r <- try (evaluate (evRun ev env expr)) :: IO (Either SomeException Double)
  case r of
    Left _ -> pure True
    Right got -> do
      printf "FAIL [%-26s] %-16s should have raised (got %g)\n" (evName ev) (show expr) got
      pure False
