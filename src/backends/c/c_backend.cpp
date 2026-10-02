// C backend: composes the translation unit and the Google Test file around
// model-written code, and applies the hygiene rules of docs/runtime.md.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string_view>

#include "seq/backend.hpp"
#include "seq/version.hpp"
#include "support/util.hpp"

namespace seq {

namespace {

constexpr std::size_t kMaxModelSourceBytes = 256 * 1024;

// Headers the compiler-owned prelude always includes.
constexpr const char* kPreludeHeaders[] = {
    "ctype.h",   "errno.h",  "float.h",  "inttypes.h", "limits.h", "math.h",
    "stdbool.h", "stddef.h", "stdint.h", "stdio.h",    "stdlib.h", "string.h",
};

// C standard headers model-written code may include itself. Headers for
// signals, non-local jumps, and threads are deliberately absent.
const std::set<std::string_view>& AllowedCHeaders() {
  static const std::set<std::string_view> headers = {
      "assert.h",      "complex.h",  "ctype.h",  "errno.h",       "fenv.h",
      "float.h",       "inttypes.h", "iso646.h", "limits.h",      "locale.h",
      "math.h",        "stdalign.h", "stdarg.h", "stdbool.h",     "stddef.h",
      "stdint.h",      "stdio.h",    "stdlib.h", "stdnoreturn.h", "string.h",
      "tgmath.h",      "time.h",     "uchar.h",  "wchar.h",       "wctype.h",
      "seq_runtime.h",
  };
  return headers;
}

const std::set<std::string_view>& AllowedTestHeaders() {
  static const std::set<std::string_view> headers = {
      "gtest/gtest.h", "algorithm", "array",       "cassert",    "cctype",
      "cerrno",        "cfloat",    "cinttypes",   "climits",    "cmath",
      "cstdarg",       "cstddef",   "cstdint",     "cstdio",     "cstdlib",
      "cstring",       "ctime",     "fstream",     "functional", "iomanip",
      "iostream",      "istream",   "iterator",    "limits",     "map",
      "memory",        "numeric",   "optional",    "ostream",    "set",
      "sstream",       "string",    "string_view", "tuple",      "utility",
      "vector",
  };
  return headers;
}

// Names that are rejected wherever they appear.
const std::set<std::string_view>& ForbiddenNames() {
  static const std::set<std::string_view> names = {
      "system",       "popen",       "pclose",     "fork",     "vfork",
      "execl",        "execlp",      "execle",     "execv",    "execvp",
      "execvpe",      "execve",      "fexecve",    "execveat", "posix_spawn",
      "posix_spawnp", "socket",      "socketpair", "dlopen",   "dlsym",
      "dlmopen",      "syscall",     "ptrace",     "asm",      "__asm",
      "__asm__",      "SEQ_NO_MAIN",
  };
  return names;
}

// Ordinary English words that are rejected only when called.
const std::set<std::string_view>& ForbiddenCalls() {
  static const std::set<std::string_view> names = {
      "connect", "bind", "listen", "accept",    "accept4",
      "clone",   "kill", "signal", "sigaction",
  };
  return names;
}

const std::set<std::string_view>& DeathTestMacros() {
  static const std::set<std::string_view> names = {
      "EXPECT_DEATH",
      "ASSERT_DEATH",
      "EXPECT_EXIT",
      "ASSERT_EXIT",
      "EXPECT_DEBUG_DEATH",
      "ASSERT_DEBUG_DEATH",
      "EXPECT_DEATH_IF_SUPPORTED",
      "ASSERT_DEATH_IF_SUPPORTED",
  };
  return names;
}

bool IsIdentStart(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsIdentPart(char c) { return IsIdentStart(c) || IsDigit(c); }

// Returns N for the identifier "seq_step_<N>", or 0.
std::size_t StepNumber(std::string_view identifier) {
  constexpr std::string_view kPrefix = "seq_step_";
  if (!StartsWith(identifier, kPrefix)) return 0;
  const std::string_view digits = identifier.substr(kPrefix.size());
  if (digits.empty() || digits.size() > 6) return 0;
  std::size_t value = 0;
  for (const char c : digits) {
    if (!IsDigit(c)) return 0;
    value = value * 10 + static_cast<std::size_t>(c - '0');
  }
  return value;
}

enum class Language { kC, kCxx };

// A deliberately small scanner: it understands comments, literals,
// preprocessor directives, identifiers, and brace nesting, which is all the
// hygiene rules need.
class Scanner {
 public:
  Scanner(std::string_view source, Language language, std::size_t step_count)
      : language_(language), step_count_(step_count) {
    Splice(source);
  }

  std::vector<PolicyViolation> Run() {
    Scan();
    if (brace_depth_ != 0) Report(0, "unbalanced braces");
    if (conditional_depth_ != 0) {
      Report(0, "unbalanced preprocessor conditionals (#if without #endif)");
    }
    if (language_ == Language::kC) {
      for (std::size_t n = 1; n <= step_count_; ++n) {
        const std::size_t count = definitions_[n];
        if (count == 0) {
          Report(0, "required function seq_step_" + std::to_string(n) +
                        " is not defined");
        }
      }
    } else if (!has_test_) {
      Report(0, "no TEST() case was written");
    }
    std::stable_sort(violations_.begin(), violations_.end(),
                     [](const PolicyViolation& a, const PolicyViolation& b) {
                       return a.line < b.line;
                     });
    return violations_;
  }

 private:
  void Report(std::size_t line, std::string message) {
    violations_.push_back(PolicyViolation{line, std::move(message)});
  }

  // Joins lines that end in a backslash, as the preprocessor does, while
  // remembering the original line of every character. Also rejects trigraphs
  // and digraphs, which would hide directives from the scan.
  void Splice(std::string_view source) {
    std::size_t line = 1;
    for (std::size_t i = 0; i < source.size(); ++i) {
      const char c = source[i];
      if (c == '?' && i + 2 < source.size() && source[i + 1] == '?' &&
          std::string_view("=/'()!<>-").find(source[i + 2]) !=
              std::string_view::npos) {
        Report(line, "trigraphs are not allowed");
      }
      if (c == '\\' && i + 1 < source.size() && source[i + 1] == '\n') {
        ++i;
        ++line;
        continue;
      }
      if (c == '\\' && i + 2 < source.size() && source[i + 1] == '\r' &&
          source[i + 2] == '\n') {
        i += 2;
        ++line;
        continue;
      }
      text_.push_back(c);
      lines_.push_back(static_cast<std::uint32_t>(line));
      if (c == '\n') ++line;
    }
  }

  std::size_t LineAt(std::size_t pos) const {
    if (lines_.empty()) return 1;
    return lines_[std::min(pos, lines_.size() - 1)];
  }

  char At(std::size_t pos) const {
    return pos < text_.size() ? text_[pos] : '\0';
  }

  std::size_t SkipSpaces(std::size_t pos) const {
    while (At(pos) == ' ' || At(pos) == '\t') ++pos;
    return pos;
  }

  std::size_t SkipToLineEnd(std::size_t pos) const {
    while (pos < text_.size() && text_[pos] != '\n') ++pos;
    return pos;
  }

  // Skips a quoted literal that starts at `pos`; returns the position after
  // it. A literal ends at its closing quote or at the end of the line.
  std::size_t SkipQuoted(std::size_t pos, char quote) const {
    ++pos;
    while (pos < text_.size() && text_[pos] != quote && text_[pos] != '\n') {
      if (text_[pos] == '\\' && pos + 1 < text_.size()) ++pos;
      ++pos;
    }
    return pos < text_.size() && text_[pos] == quote ? pos + 1 : pos;
  }

  // Skips a C++ raw string literal whose opening quote is at `pos`.
  std::size_t SkipRawString(std::size_t pos) const {
    const std::size_t open = text_.find('(', pos);
    if (open == std::string::npos) return text_.size();
    const std::string closing =
        ")" + text_.substr(pos + 1, open - pos - 1) + "\"";
    const std::size_t end = text_.find(closing, open);
    return end == std::string::npos ? text_.size() : end + closing.size();
  }

  void CheckInclude(std::size_t pos, std::size_t line) {
    pos = SkipSpaces(pos);
    const char open = At(pos);
    if (open != '<' && open != '"') {
      Report(line, "#include must name a header directly");
      return;
    }
    const char close = open == '<' ? '>' : '"';
    const std::size_t end = text_.find(close, pos + 1);
    const std::size_t line_end = SkipToLineEnd(pos);
    if (end == std::string::npos || end > line_end) {
      Report(line, "malformed #include");
      return;
    }
    const std::string name = text_.substr(pos + 1, end - pos - 1);
    bool allowed = AllowedCHeaders().count(name) != 0;
    if (!allowed && language_ == Language::kCxx) {
      allowed = AllowedTestHeaders().count(name) != 0;
    }
    if (!allowed) {
      Report(line, "header <" + name + "> is not on the include allowlist");
    }
  }

  // Handles the directive whose '#' is at `pos`. Returns the position from
  // which scanning continues.
  std::size_t Directive(std::size_t pos) {
    const std::size_t line = LineAt(pos);
    std::size_t cursor = SkipSpaces(pos + 1);
    std::size_t end = cursor;
    while (IsIdentPart(At(end))) ++end;
    const std::string_view name =
        std::string_view(text_).substr(cursor, end - cursor);

    if (name == "include") {
      CheckInclude(end, line);
      return SkipToLineEnd(end);
    }
    if (name == "include_next" || name == "embed" || name == "import" ||
        name == "pragma" || name == "line") {
      Report(line, "#" + std::string(name) + " is not allowed");
      return SkipToLineEnd(end);
    }
    if (name == "if" || name == "ifdef" || name == "ifndef") {
      ++conditional_depth_;
    } else if (name == "endif") {
      if (--conditional_depth_ < 0) {
        Report(line, "#endif without a matching #if");
        conditional_depth_ = 0;
      }
    }
    // The rest of the line is scanned as ordinary tokens, so a forbidden name
    // in a macro body is still found.
    return end;
  }

  void Identifier(std::string_view word, std::size_t pos, std::size_t after) {
    const std::size_t line = LineAt(pos);
    const std::size_t next = SkipSpaces(after);
    const bool called = At(next) == '(';

    if (word == "main") {
      Report(line,
             "main must not be defined or referenced; the compiler "
             "supplies it");
    } else if (ForbiddenNames().count(word) != 0) {
      Report(line, "'" + std::string(word) + "' is not allowed");
    } else if (called && ForbiddenCalls().count(word) != 0) {
      Report(line, "calling '" + std::string(word) + "' is not allowed");
    } else if (language_ == Language::kCxx &&
               DeathTestMacros().count(word) != 0) {
      Report(line, "death tests are not allowed: '" + std::string(word) +
                       "' needs to start a process, which the sandbox denies");
    }
    if (language_ == Language::kCxx && called &&
        (word == "TEST" || word == "TEST_F" || word == "TEST_P")) {
      has_test_ = true;
    }

    if (language_ == Language::kC) {
      const std::size_t number = StepNumber(word);
      if (number != 0) {
        if (number > step_count_) {
          Report(line, "unexpected function " + std::string(word) +
                           ": the workflow has " + std::to_string(step_count_) +
                           " step(s)");
        } else if (brace_depth_ == 0 && paren_depth_ == 0 && called) {
          pending_step_ = number;
          pending_line_ = line;
          pending_state_ = Pending::kInParameters;
        }
      }
    }
  }

  void Punctuation(char c, std::size_t pos) {
    switch (c) {
      case '(':
        ++paren_depth_;
        break;
      case ')':
        if (paren_depth_ > 0) --paren_depth_;
        if (pending_state_ == Pending::kInParameters && paren_depth_ == 0) {
          pending_state_ = Pending::kAfterParameters;
          return;
        }
        break;
      case '{':
        if (pending_state_ == Pending::kAfterParameters) {
          if (++definitions_[pending_step_] > 1) {
            Report(pending_line_, "function seq_step_" +
                                      std::to_string(pending_step_) +
                                      " is defined more than once");
          }
        }
        ++brace_depth_;
        break;
      case '}':
        if (--brace_depth_ < 0) {
          Report(LineAt(pos), "unbalanced braces");
          brace_depth_ = 0;
        }
        break;
      default:
        break;
    }
    if (pending_state_ == Pending::kAfterParameters) {
      pending_state_ = Pending::kNone;
    }
  }

  void Scan() {
    bool line_start = true;
    std::size_t pos = 0;
    while (pos < text_.size()) {
      const char c = text_[pos];
      if (c == '\n') {
        line_start = true;
        ++pos;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
        ++pos;
        continue;
      }
      if (c == '/' && At(pos + 1) == '/') {
        pos = SkipToLineEnd(pos);
        continue;
      }
      if (c == '/' && At(pos + 1) == '*') {
        const std::size_t end = text_.find("*/", pos + 2);
        if (end == std::string::npos) {
          Report(LineAt(pos), "unterminated comment");
          return;
        }
        pos = end + 2;
        continue;
      }
      if ((c == '%' && At(pos + 1) == ':') ||
          (c == '<' && At(pos + 1) == '%') ||
          (c == '%' && At(pos + 1) == '>')) {
        Report(LineAt(pos), "digraphs are not allowed");
        pos += 2;
        line_start = false;
        continue;
      }
      if (c == '#') {
        if (line_start) {
          pos = Directive(pos);
        } else {
          if (At(pos + 1) == '#') {
            Report(LineAt(pos), "token pasting (##) is not allowed");
            ++pos;
          }
          ++pos;
        }
        line_start = false;
        continue;
      }
      line_start = false;
      if (c == '"') {
        pos = SkipQuoted(pos, '"');
        continue;
      }
      if (c == '\'') {
        pos = SkipQuoted(pos, '\'');
        continue;
      }
      if (IsDigit(c)) {
        // A number, including C++ digit separators such as 1'000.
        ++pos;
        while (IsIdentPart(At(pos)) || At(pos) == '.' ||
               (At(pos) == '\'' && IsIdentPart(At(pos + 1)))) {
          ++pos;
        }
        continue;
      }
      if (IsIdentStart(c)) {
        std::size_t end = pos;
        while (IsIdentPart(At(end))) ++end;
        const std::string_view word =
            std::string_view(text_).substr(pos, end - pos);
        if (At(end) == '"') {
          // String literal with an encoding prefix; R marks a raw string.
          if (language_ == Language::kCxx &&
              (word == "R" || word == "u8R" || word == "uR" || word == "UR" ||
               word == "LR")) {
            pos = SkipRawString(end);
            continue;
          }
          if (word == "u8" || word == "u" || word == "U" || word == "L") {
            pos = SkipQuoted(end, '"');
            continue;
          }
        }
        Identifier(word, pos, end);
        pos = end;
        continue;
      }
      Punctuation(c, pos);
      ++pos;
    }
  }

  enum class Pending { kNone, kInParameters, kAfterParameters };

  Language language_;
  std::size_t step_count_;
  std::string text_;
  std::vector<std::uint32_t> lines_;
  std::vector<PolicyViolation> violations_;
  int brace_depth_ = 0;
  int paren_depth_ = 0;
  int conditional_depth_ = 0;
  bool has_test_ = false;
  Pending pending_state_ = Pending::kNone;
  std::size_t pending_step_ = 0;
  std::size_t pending_line_ = 0;
  std::map<std::size_t, std::size_t> definitions_;
};

std::vector<PolicyViolation> CheckSource(const std::string& source,
                                         Language language,
                                         std::size_t step_count) {
  if (source.size() > kMaxModelSourceBytes) {
    return {PolicyViolation{
        0, "generated source is " + FormatBytes(source.size()) +
               ": the limit is " + FormatBytes(kMaxModelSourceBytes)}};
  }
  if (source.find('\0') != std::string::npos || !IsValidUtf8(source)) {
    return {PolicyViolation{0, "generated source is not valid UTF-8 text"}};
  }
  return Scanner(source, language, step_count).Run();
}

std::string StepPrototypes(const Workflow& workflow) {
  std::string out;
  for (std::size_t i = 0; i < workflow.steps.size(); ++i) {
    out += "int seq_step_" + std::to_string(i + 1) + "(seq_ctx *ctx);\n";
  }
  return out;
}

std::string EnsureTrailingNewline(std::string text) {
  if (text.empty() || text.back() != '\n') text.push_back('\n');
  return text;
}

class CBackend : public Backend {
 public:
  explicit CBackend(std::string runtime_header)
      : runtime_header_(std::move(runtime_header)) {}

  std::string Name() const override { return "C"; }

  std::vector<std::string> AllowedDependencies() const override {
    return {"libc", "libm", "seqrt"};
  }

  std::string StepContract(const Workflow& workflow) const override {
    std::string headers;
    for (const char* header : kPreludeHeaders) {
      headers += std::string(" <") + header + ">";
    }
    std::string functions;
    for (std::size_t i = 0; i < workflow.steps.size(); ++i) {
      functions += "  int seq_step_" + std::to_string(i + 1) +
                   "(seq_ctx *ctx)    /* step \"" + workflow.steps[i].name +
                   "\" */\n";
    }
    return "The program is one C17 translation unit. You write only the step "
           "functions.\n"
           "\n"
           "Already provided before your code, so do not repeat it:\n"
           "  #include \"seq_runtime.h\" and" +
           headers +
           "\n"
           "  a prototype for every step function\n"
           "Provided after your code: main(), which calls the step functions "
           "in order and stops at the first one that returns nonzero.\n"
           "\n"
           "Define exactly these functions, each exactly once:\n" +
           functions +
           "\n"
           "Rules:\n"
           "- Do not define main.\n"
           "- Return 0 on success. On failure use: return seq_fail(ctx, "
           "\"message\");\n"
           "- Create output files only with seq_output_open(relpath, mode). "
           "Read input files only with seq_input_open(relpath). Check every "
           "result for NULL and fclose every file.\n"
           "- Print text results with printf.\n"
           "- Share data between steps with file-scope static variables or "
           "with files written by an earlier step.\n"
           "- Helper functions and types are allowed; declare helpers "
           "static.\n"
           "- You may #include other C standard headers. Nothing else can be "
           "included.\n"
           "- Do not use system, popen, fork, exec, sockets, signals, "
           "dlopen, or inline assembly. The program has no network and "
           "cannot start processes.\n"
           "- To draw a chart call seq_chart_bar_png. Do not write image "
           "encoding code.\n"
           "\n"
           "Runtime API (seq_runtime.h):\n" +
           runtime_header_;
  }

  std::string TestContract(const Workflow& workflow) const override {
    return "The tests are one C++17 file using Google Test. You write only "
           "the TEST cases.\n"
           "\n"
           "Already provided before your code, so do not repeat it:\n"
           "  #include <gtest/gtest.h> <cmath> <cstdio> <cstdlib> <cstring> "
           "<fstream> <sstream> <string> <vector>\n"
           "  extern \"C\" declarations of seq_runtime.h and of:\n" +
           StepPrototypes(workflow) +
           "main() is provided by gtest_main.\n"
           "\n"
           "Rules:\n"
           "- Write TEST(SuiteName, TestName) cases only. Do not define "
           "main.\n"
           "- Call a step like this:\n"
           "    seq_ctx ctx;\n"
           "    seq_ctx_init(&ctx);\n"
           "    EXPECT_EQ(0, seq_step_1(&ctx));\n"
           "- Steps depend on earlier steps. In every test call the steps in "
           "order starting from seq_step_1.\n"
           "- Tests run in an empty working directory. Files a step creates "
           "with seq_output_open appear there and can be checked with "
           "std::ifstream.\n"
           "- Do not use death tests (EXPECT_DEATH, ASSERT_DEATH, "
           "EXPECT_EXIT, ASSERT_EXIT): the test process cannot start other "
           "processes.\n"
           "- Do not use system, popen, fork, exec, sockets, or threads.\n";
  }

  std::string ComposeUnit(const Workflow& workflow,
                          const std::string& step_functions) const override {
    std::string out;
    out += "/* Generated by seqc " + std::string(kVersion) +
           " for workflow \"" + workflow.name +
           "\".\n"
           " * This file is rebuilt whenever the workflow, its inputs, or the "
           "toolchain\n"
           " * change; edits made here are not kept. */\n"
           "\n"
           "/* ---- Compiler-owned prelude ---- */\n"
           "#include \"seq_runtime.h\"\n"
           "\n";
    for (const char* header : kPreludeHeaders) {
      out += std::string("#include <") + header + ">\n";
    }
    out += "\n" + StepPrototypes(workflow) +
           "\n"
           "/* ---- Step functions (model-written) ---- */\n";
    out += EnsureTrailingNewline(step_functions);
    out +=
        "/* ---- Compiler-owned driver ---- */\n"
        "#ifndef SEQ_NO_MAIN\n"
        "static const seq_step_entry seq_steps[] = {\n";
    for (std::size_t i = 0; i < workflow.steps.size(); ++i) {
      out += "    {\"" + workflow.steps[i].name + "\", seq_step_" +
             std::to_string(i + 1) + "},\n";
    }
    out +=
        "};\n"
        "\n"
        "int main(void) {\n"
        "  return seq_run(seq_steps, " +
        std::to_string(workflow.steps.size()) +
        ");\n"
        "}\n"
        "#endif /* SEQ_NO_MAIN */\n";
    return out;
  }

  std::vector<PolicyViolation> CheckStepFunctions(
      const Workflow& workflow,
      const std::string& step_functions) const override {
    return CheckSource(step_functions, Language::kC, workflow.steps.size());
  }

  std::string ComposeTest(const Workflow& workflow,
                          const std::string& test_cases) const override {
    std::string out;
    out += "// Generated by seqc " + std::string(kVersion) +
           " for workflow \"" + workflow.name +
           "\".\n"
           "// Tests of the generated step functions. Rebuilt with the "
           "workflow.\n"
           "\n"
           "// ---- Compiler-owned prelude ----\n"
           "#include <gtest/gtest.h>\n"
           "\n"
           "#include <cmath>\n"
           "#include <cstdio>\n"
           "#include <cstdlib>\n"
           "#include <cstring>\n"
           "#include <fstream>\n"
           "#include <sstream>\n"
           "#include <string>\n"
           "#include <vector>\n"
           "\n"
           "extern \"C\" {\n"
           "#include \"seq_runtime.h\"\n" +
           StepPrototypes(workflow) +
           "}\n"
           "\n"
           "// ---- Test cases (model-written) ----\n";
    out += EnsureTrailingNewline(test_cases);
    return out;
  }

  std::vector<PolicyViolation> CheckTestCases(
      const std::string& test_cases) const override {
    return CheckSource(test_cases, Language::kCxx, 0);
  }

 private:
  std::string runtime_header_;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend(BackendKind kind,
                                       std::string runtime_header) {
  switch (kind) {
    case BackendKind::kC:
      return std::make_unique<CBackend>(std::move(runtime_header));
  }
  return nullptr;
}

std::string DescribeViolations(const std::vector<PolicyViolation>& violations,
                               const std::string& source) {
  const std::vector<std::string> lines = SplitLines(source);
  std::string out;
  for (const PolicyViolation& violation : violations) {
    if (violation.line == 0) {
      out += "policy: " + violation.message + "\n";
      continue;
    }
    out += "policy: line " + std::to_string(violation.line) + ": " +
           violation.message + "\n";
    if (violation.line <= lines.size()) {
      out += "    " + std::string(TrimWhitespace(lines[violation.line - 1])) +
             "\n";
    }
  }
  return out;
}

}  // namespace seq
