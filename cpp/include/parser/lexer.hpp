#pragma once

#include <string_view>
#include <vector>

#include "parser/token.hpp"

namespace mp {

// Shared tokenizer used by every parsing strategy, so the comparison
// measures the parsing approach rather than differences in lexing.
//
// Always appends a trailing TokenType::End token.
// Throws std::runtime_error on an unrecognized character.
std::vector<Token> tokenize(std::string_view src);

}  // namespace mp
