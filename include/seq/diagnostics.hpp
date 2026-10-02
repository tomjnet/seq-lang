#ifndef SEQ_DIAGNOSTICS_HPP_
#define SEQ_DIAGNOSTICS_HPP_

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace seq {

// 1-based position in a source file. Columns count Unicode code points.
struct SourceLocation {
  std::size_t line = 1;
  std::size_t column = 1;
};

// Source text after normalization: no byte-order mark, LF line endings.
struct SourceText {
  // Path as the user wrote it, used in diagnostics.
  std::string path;
  std::string text;

  // Returns line `line` (1-based) without its newline, or an empty view.
  std::string_view Line(std::size_t line) const;
};

struct Diagnostic {
  // Stable code such as "E0304". docs/language.md lists every code.
  std::string code;
  SourceLocation location;
  std::string message;
  // Optional second line that says how to fix the problem.
  std::string hint;
};

class Diagnostics {
 public:
  void Error(std::string code, SourceLocation location, std::string message,
             std::string hint = "");

  bool HasErrors() const { return !items_.empty(); }
  const std::vector<Diagnostic>& items() const { return items_; }

  // Prints every diagnostic as
  //   path:line:column: error[E0000]: message
  // followed by the source line and a caret.
  void Print(std::ostream& out, const SourceText& source, bool color) const;

 private:
  std::vector<Diagnostic> items_;
};

}  // namespace seq

#endif  // SEQ_DIAGNOSTICS_HPP_
