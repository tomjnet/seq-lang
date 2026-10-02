#include "lexer/lexer.hpp"

#include <algorithm>
#include <cstdio>

#include "seq/ast.hpp"
#include "support/util.hpp"

namespace seq {

namespace {

bool IsIdentifierStart(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsIdentifierPart(char c) { return IsIdentifierStart(c) || IsDigit(c); }

bool LineHasError(const Diagnostics& diagnostics, std::size_t line) {
  for (const Diagnostic& item : diagnostics.items()) {
    if (item.location.line == line) return true;
  }
  return false;
}

std::string DescribeByte(unsigned char c) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%02X", c);
  return buffer;
}

}  // namespace

bool IsReservedWord(std::string_view word) {
  return word == "model" || word == "backend" || word == "name" ||
         word == "step" || word == "ask";
}

bool NormalizeSource(std::string_view raw, std::string display_path,
                     SourceText* out, Diagnostics* diagnostics) {
  out->path = std::move(display_path);
  out->text.clear();

  if (raw.size() > kMaxSourceBytes) {
    diagnostics->Error(
        "E0109", {1, 1},
        "source file is larger than " + FormatBytes(kMaxSourceBytes));
    return false;
  }
  if (StartsWith(raw, "\xEF\xBB\xBF")) raw.remove_prefix(3);

  out->text.reserve(raw.size());
  std::size_t line = 1;
  std::size_t column = 1;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    const unsigned char c = static_cast<unsigned char>(raw[pos]);
    if (c == '\r') {
      if (pos + 1 < raw.size() && raw[pos + 1] == '\n') {
        ++pos;  // CRLF: keep only the LF, handled below.
        continue;
      }
      diagnostics->Error("E0104", {line, column},
                         "carriage return without a following line feed",
                         "use LF or CRLF line endings");
      out->text.push_back('\n');
      ++line;
      column = 1;
      ++pos;
      continue;
    }
    if (c == '\n') {
      out->text.push_back('\n');
      ++line;
      column = 1;
      ++pos;
      continue;
    }
    if (c == '\t') {
      diagnostics->Error("E0103", {line, column}, "tabs are not allowed",
                         "indent with four spaces; write \\t inside a string");
      out->text.push_back(' ');
      ++column;
      ++pos;
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      diagnostics->Error(
          "E0102", {line, column},
          c == 0 ? std::string("NUL byte in source file")
                 : "control character " + DescribeByte(c) + " in source file");
      out->text.push_back(' ');
      ++column;
      ++pos;
      continue;
    }
    const std::size_t length = Utf8SequenceLength(raw, pos);
    if (length == 0) {
      diagnostics->Error(
          "E0101", {line, column},
          "source file is not valid UTF-8 (byte " + DescribeByte(c) + ")");
      return false;
    }
    out->text.append(raw.substr(pos, length));
    ++column;
    pos += length;
  }
  return true;
}

std::vector<SourceLine> Lex(const SourceText& source,
                            Diagnostics* diagnostics) {
  std::vector<SourceLine> lines;
  const std::string_view text = source.text;
  std::size_t line_number = 0;
  std::size_t line_start = 0;

  while (line_start <= text.size()) {
    std::size_t line_end = text.find('\n', line_start);
    const bool last = line_end == std::string_view::npos;
    if (last) line_end = text.size();
    const std::string_view line =
        text.substr(line_start, line_end - line_start);
    ++line_number;
    line_start = line_end + 1;

    std::size_t pos = 0;
    while (pos < line.size() && line[pos] == ' ') ++pos;
    const std::size_t indent = pos;
    if (pos == line.size() || line[pos] == '#') {
      if (last) break;
      continue;
    }

    SourceLine current;
    current.number = line_number;
    current.indent = indent;
    // A line that already has a normalization error (a tab, a control
    // character) is reported once and not tokenized.
    current.has_error = LineHasError(*diagnostics, line_number);

    std::size_t column = indent + 1;
    const auto advance = [&](std::size_t bytes) {
      column += Utf8Length(line.substr(pos, bytes));
      pos += bytes;
    };

    while (!current.has_error && pos < line.size()) {
      const char c = line[pos];
      if (c == ' ') {
        advance(1);
        continue;
      }
      if (c == '#') break;

      Token token;
      token.location = {line_number, column};
      if (IsIdentifierStart(c)) {
        std::size_t end = pos;
        while (end < line.size() && IsIdentifierPart(line[end])) ++end;
        token.kind = TokenKind::kIdentifier;
        token.text = std::string(line.substr(pos, end - pos));
        advance(end - pos);
      } else if (IsDigit(c)) {
        std::size_t end = pos;
        while (end < line.size() &&
               (IsIdentifierPart(line[end]) || line[end] == '.')) {
          ++end;
        }
        token.kind = TokenKind::kNumber;
        token.text = std::string(line.substr(pos, end - pos));
        advance(end - pos);
      } else if (c == '"') {
        token.kind = TokenKind::kString;
        advance(1);
        bool closed = false;
        while (pos < line.size()) {
          const char s = line[pos];
          if (s == '"') {
            advance(1);
            closed = true;
            break;
          }
          if (s == '\\') {
            const SourceLocation escape_location = {line_number, column};
            if (pos + 1 >= line.size()) {
              advance(1);
              break;  // Reported as an unterminated string below.
            }
            const char e = line[pos + 1];
            if (e == '"') {
              token.text.push_back('"');
            } else if (e == '\\') {
              token.text.push_back('\\');
            } else if (e == 'n') {
              token.text.push_back('\n');
            } else if (e == 't') {
              token.text.push_back('\t');
            } else {
              const std::size_t length =
                  std::max<std::size_t>(Utf8SequenceLength(line, pos + 1), 1);
              diagnostics->Error("E0106", escape_location,
                                 "unknown escape sequence '\\" +
                                     std::string(line.substr(pos + 1, length)) +
                                     "'",
                                 "supported escapes are \\\" \\\\ \\n and \\t");
              current.has_error = true;
              advance(1 + length);
              continue;
            }
            advance(2);
            continue;
          }
          const std::size_t length =
              std::max<std::size_t>(Utf8SequenceLength(line, pos), 1);
          token.text.append(line.substr(pos, length));
          advance(length);
        }
        if (!closed) {
          diagnostics->Error("E0105", token.location,
                             "unterminated string literal",
                             "strings are single-line and end with \"");
          current.has_error = true;
          break;
        }
      } else {
        switch (c) {
          case '(':
            token.kind = TokenKind::kLeftParen;
            break;
          case ')':
            token.kind = TokenKind::kRightParen;
            break;
          case ':':
            token.kind = TokenKind::kColon;
            break;
          case '=':
            token.kind = TokenKind::kEquals;
            break;
          case '.':
            token.kind = TokenKind::kDot;
            break;
          case ',':
            token.kind = TokenKind::kComma;
            break;
          default: {
            const std::size_t length =
                std::max<std::size_t>(Utf8SequenceLength(line, pos), 1);
            diagnostics->Error("E0107", token.location,
                               "unexpected character '" +
                                   std::string(line.substr(pos, length)) + "'");
            current.has_error = true;
            break;
          }
        }
        if (current.has_error) break;
        token.text = std::string(1, c);
        advance(1);
      }
      current.tokens.push_back(std::move(token));
      current.end = {line_number, column};
    }

    if (current.tokens.empty() && !current.has_error) {
      current.end = {line_number, indent + 1};
    }
    lines.push_back(std::move(current));
    if (last) break;
  }
  return lines;
}

}  // namespace seq
