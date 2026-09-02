{-# LANGUAGE BangPatterns #-}

-- | Shared tokenizer — one grammar for every strategy (mirrors cpp/src/lexer.cpp).
module MathParser.Lexer
  ( Kind(..)
  , Tok(..)
  , tokenize
  , spanNumber
  , readNum
  ) where

import Data.Char (isDigit, isSpace, isAlpha, isAlphaNum, toLower, ord, digitToInt)
import Data.List (foldl')

data Kind
  = KNum | KVar | KPlus | KMinus | KStar | KSlash | KCaret
  | KLParen | KRParen | KEnd
  deriving (Eq, Show)

-- | A token. 'tVal' holds a number's value or a variable's index (0..25).
data Tok = Tok { tKind :: !Kind, tVal :: !Double, tPos :: !Int }
  deriving Show

tokenize :: String -> [Tok]
tokenize = go 0
  where
    go !i [] = [Tok KEnd 0 i]
    go !i (c:cs)
      | isSpace c = go (i + 1) cs
      | isDigit c || c == '.' =
          let (lexeme, rest) = spanNumber (c : cs)
              n = length lexeme
          in Tok KNum (readNum lexeme i) i : go (i + n) rest
      | isAlpha c || c == '_' =
          let (word, rest) = span (\x -> isAlphaNum x || x == '_') (c : cs)
          in if length word /= 1 || c == '_'
               then errorAt i ("unknown identifier")
               else Tok KVar (fromIntegral (ord (toLower c) - ord 'a')) i
                      : go (i + 1) rest
      | otherwise = case c of
          '+' -> Tok KPlus   0 i : go (i + 1) cs
          '-' -> Tok KMinus  0 i : go (i + 1) cs
          '*' -> Tok KStar   0 i : go (i + 1) cs
          '/' -> Tok KSlash  0 i : go (i + 1) cs
          '^' -> Tok KCaret  0 i : go (i + 1) cs
          '(' -> Tok KLParen 0 i : go (i + 1) cs
          ')' -> Tok KRParen 0 i : go (i + 1) cs
          _   -> errorAt i ("unexpected character " ++ [c])

errorAt :: Int -> String -> a
errorAt i msg
  | i < 0     = error msg   -- caller tracks no position (direct-scannerless)
  | otherwise = error (msg ++ " at position " ++ show i)

-- | A numeric run: digits, optional fraction, optional exponent.
spanNumber :: String -> (String, String)
spanNumber s =
  let (intp, r1)  = span isDigit s
      (fracp, r2) = case r1 of
                      ('.' : t) -> let (d, r) = span isDigit t in ('.' : d, r)
                      _         -> ("", r1)
      (expp, r3)  = case r2 of
                      (e : t) | e `elem` "eE" ->
                        let (sgn, t1) = case t of
                                          (x : u) | x `elem` "+-" -> ([x], u)
                                          _                       -> ("", t)
                            (d, r) = span isDigit t1
                        in if null d then ("", r2) else (e : sgn ++ d, r)
                      _ -> ("", r2)
  in (intp ++ fracp ++ expp, r3)

readNum :: String -> Int -> Double
readNum lexeme i
  -- A lexeme with no digits (a bare ".") is not a number; without this guard
  -- the padding below would turn it into "0.0" and silently accept it.
  | not (any isDigit lexeme) = errorAt i "invalid number"
  | Just v <- fastPath lexeme = v
  | otherwise = slowPath lexeme i

-- Clinger's fast path — the first thing from_chars/strtod try, and the road
-- every corpus literal takes: a mantissa of at most 15 digits (< 2^53, so
-- exact as a Double) scaled by a power of ten up to 10^22 (also exact) is ONE
-- correctly rounded IEEE operation, so the result is bit-identical to what
-- `reads` produces via Rational — at a small fraction of the cost. Anything
-- longer, or with a larger exponent (1e400 -> inf, 1e-400 -> 0), takes the
-- slow road below. The same rule the C++ lexer follows (see lexer.hpp).
fastPath :: String -> Maybe Double
fastPath s =
  let (intp, r1)  = span isDigit s
      (fracp, r2) = case r1 of
                      ('.' : t) -> span isDigit t
                      _         -> ("", r1)
      (expSign, expDigits) = case r2 of
                      (e : '-' : d) | e `elem` "eE" -> (negate, d)
                      (e : '+' : d) | e `elem` "eE" -> (id, d)
                      (e : d)       | e `elem` "eE" -> (id, d)
                      _                             -> (id, "")
      digits = intp ++ fracp
      k      = expSign (digitsToInt expDigits) - length fracp
  in if length digits <= 15 && length expDigits <= 4 && abs k <= 22
       then let m = fromIntegral (digitsToInt digits) :: Double
            in Just (if k >= 0 then m * pow10 k else m / pow10 (negate k))
       else Nothing
  where
    digitsToInt :: String -> Int
    digitsToInt = foldl' (\a c -> a * 10 + digitToInt c) 0
    pow10 :: Int -> Double
    pow10 k = 10 ^ k   -- exact for k <= 22 (every intermediate is 2^a * 5^b, 5^b < 2^53)

slowPath :: String -> Int -> Double
slowPath lexeme i =
  -- Pad a leading/trailing '.' so Haskell's `reads` accepts it (".5" -> "0.5",
  -- "2." -> "2.0").
  let padded = pad lexeme
  in case reads padded :: [(Double, String)] of
       [(v, "")] -> v
       _         -> errorAt i "invalid number"
  where
    pad l  = padTrail (padLead l)
    padLead l = if take 1 l == "." then '0' : l else l
    padTrail l = case break (`elem` "eE") l of
                   (mant, expo) | take 1 (reverse mant) == "." -> mant ++ "0" ++ expo
                   _                                           -> l
