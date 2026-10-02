#ifndef SEQ_PARSER_PARSER_HPP_
#define SEQ_PARSER_PARSER_HPP_

#include <vector>

#include "lexer/lexer.hpp"
#include "seq/ast.hpp"
#include "seq/diagnostics.hpp"

namespace seq {

// Builds the syntax tree from lexed lines. Syntax errors are reported and the
// offending line is skipped, so one run reports as many errors as it can.
Program Parse(const std::vector<SourceLine>& lines, Diagnostics* diagnostics);

}  // namespace seq

#endif  // SEQ_PARSER_PARSER_HPP_
