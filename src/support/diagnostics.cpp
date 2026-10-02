#include "seq/diagnostics.hpp"

#include <algorithm>

namespace seq {

std::string_view SourceText::Line(std::size_t line) const {
  if (line == 0) return {};
  std::size_t start = 0;
  for (std::size_t current = 1; current < line; ++current) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string::npos) return {};
    start = newline + 1;
  }
  if (start > text.size()) return {};
  std::size_t end = text.find('\n', start);
  if (end == std::string::npos) end = text.size();
  return std::string_view(text).substr(start, end - start);
}

void Diagnostics::Error(std::string code, SourceLocation location,
                        std::string message, std::string hint) {
  items_.push_back(Diagnostic{std::move(code), location, std::move(message),
                              std::move(hint)});
}

void Diagnostics::Print(std::ostream& out, const SourceText& source,
                        bool color) const {
  const char* bold = color ? "\033[1m" : "";
  const char* red = color ? "\033[1;31m" : "";
  const char* green = color ? "\033[1;32m" : "";
  const char* cyan = color ? "\033[36m" : "";
  const char* reset = color ? "\033[0m" : "";

  std::vector<const Diagnostic*> ordered;
  ordered.reserve(items_.size());
  for (const Diagnostic& item : items_) ordered.push_back(&item);
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Diagnostic* a, const Diagnostic* b) {
                     if (a->location.line != b->location.line) {
                       return a->location.line < b->location.line;
                     }
                     return a->location.column < b->location.column;
                   });

  for (const Diagnostic* item : ordered) {
    out << bold << source.path << ":" << item->location.line << ":"
        << item->location.column << ": " << reset << red << "error["
        << item->code << "]" << reset << bold << ": " << item->message << reset
        << "\n";
    const std::string_view line = source.Line(item->location.line);
    if (!line.empty() || item->location.column == 1) {
      out << "    " << line << "\n";
      out << "    "
          << std::string(
                 item->location.column > 0 ? item->location.column - 1 : 0, ' ')
          << green << "^" << reset << "\n";
    }
    if (!item->hint.empty()) {
      out << "    " << cyan << "hint: " << reset << item->hint << "\n";
    }
  }
}

}  // namespace seq
