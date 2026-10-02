#ifndef SEQ_LEXER_LEXER_HPP_
#define SEQ_LEXER_LEXER_HPP_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "seq/diagnostics.hpp"

namespace seq {

enum class TokenKind {
  kIdentifier,
  kString,
  kNumber,
  kLeftParen,
  kRightParen,
  kColon,
  kEquals,
  kDot,
  kComma,
};

struct Token {
  TokenKind kind = TokenKind::kIdentifier;
  // Decoded value for a string, source spelling for everything else.
  std::string text;
  SourceLocation location;
};

// One logical line that carries tokens. Blank and comment-only lines are not
// reported: they have no indentation meaning.
struct SourceLine {
  std::size_t number = 0;
  // Count of leading spaces.
  std::size_t indent = 0;
  std::vector<Token> tokens;
  // Location just past the last token, for "expected X" messages.
  SourceLocation end;
  // A lexical error was reported on this line; the parser skips it.
  bool has_error = false;
};

// Normalizes raw file bytes: drops one leading byte-order mark, converts CRLF
// to LF, and rejects invalid UTF-8, NUL, tabs, a lone CR, and other control
// characters. Returns false if the text cannot be lexed at all.
bool NormalizeSource(std::string_view raw, std::string display_path,
                     SourceText* out, Diagnostics* diagnostics);

std::vector<SourceLine> Lex(const SourceText& source, Diagnostics* diagnostics);

// model, backend, name, step, and ask.
bool IsReservedWord(std::string_view word);

}  // namespace seq

#endif  // SEQ_LEXER_LEXER_HPP_
