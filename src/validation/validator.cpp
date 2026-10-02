#include "validation/validator.hpp"

#include <cstddef>
#include <map>

#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "support/util.hpp"

namespace seq {

namespace {

constexpr char kModelPrefix[] = "https://huggingface.co/";
constexpr char kModelForm[] =
    "use https://huggingface.co/<owner>/<repo>, optionally followed by "
    "/tree/<revision>";

bool IsRepoSegment(std::string_view segment) {
  if (segment.empty() || segment.size() > 96) return false;
  if (segment == "." || segment == "..") return false;
  for (const char c : segment) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

bool IsBlank(std::string_view text) { return TrimWhitespace(text).empty(); }

template <typename Decl>
void ReportDuplicates(const std::vector<Decl>& decls, const char* what,
                      Diagnostics* diagnostics) {
  for (std::size_t i = 1; i < decls.size(); ++i) {
    diagnostics->Error(
        "E0304", decls[i].location,
        std::string("duplicate ") + what + " declaration",
        "first declared on line " + std::to_string(decls[0].location.line));
  }
}

void ValidateModel(const Program& program, Diagnostics* diagnostics,
                   Workflow* workflow) {
  if (program.models.empty()) {
    diagnostics->Error(
        "E0301", {1, 1}, "missing required model declaration",
        std::string("add model(\"") + kReferenceModelUrl + "\")");
    return;
  }
  ReportDuplicates(program.models, "model", diagnostics);
  const ModelDecl& decl = program.models.front();
  if (decl.arguments.empty()) {
    diagnostics->Error(
        "E0307", decl.location, "model() requires one string argument",
        "write model(\"https://huggingface.co/<owner>/<repo>\")");
    return;
  }
  if (decl.arguments.size() > 2) {
    diagnostics->Error("E0307", decl.arguments[2].location,
                       "model() takes one argument, found " +
                           std::to_string(decl.arguments.size()));
    return;
  }
  for (const Argument& argument : decl.arguments) {
    if (argument.kind != Argument::Kind::kString) {
      diagnostics->Error("E0308", argument.location,
                         "model() arguments must be string literals");
      return;
    }
    if (IsBlank(argument.value)) {
      diagnostics->Error("E0309", argument.location,
                         "model() argument must not be empty");
      return;
    }
  }
  if (decl.arguments.size() == 2) {
    diagnostics->Error(
        "E0310", decl.arguments[1].location,
        "model server argument is not supported in v0.1",
        "remove the second argument; the inference runtime is set in the "
        "user configuration");
    return;
  }
  ModelRef model;
  if (!ParseModelUrl(decl.arguments[0].value, &model)) {
    diagnostics->Error("E0311", decl.arguments[0].location,
                       "model must be a Hugging Face repository URL",
                       kModelForm);
    return;
  }
  model.location = decl.location;
  workflow->model = std::move(model);
}

void ValidateBackend(const Program& program, Diagnostics* diagnostics,
                     Workflow* workflow) {
  if (program.backends.empty()) {
    diagnostics->Error("E0302", {1, 1}, "missing required backend declaration",
                       "add backend.C()");
    return;
  }
  ReportDuplicates(program.backends, "backend", diagnostics);
  const BackendDecl& decl = program.backends.front();
  if (decl.selector == "Rust") {
    diagnostics->Error("E0305", decl.selector_location,
                       "backend.Rust() is not supported in v0.1",
                       "use backend.C()");
    return;
  }
  if (decl.selector != "C") {
    diagnostics->Error("E0306", decl.selector_location,
                       "unknown backend '" + decl.selector + "'",
                       "use backend.C()");
    return;
  }
  if (!decl.arguments.empty()) {
    diagnostics->Error("E0320", decl.arguments.front().location,
                       "backend.C() takes no arguments in v0.1");
    return;
  }
  workflow->backend = BackendKind::kC;
}

void ValidateName(const Program& program, Diagnostics* diagnostics,
                  Workflow* workflow) {
  if (program.names.empty()) {
    diagnostics->Error("E0303", {1, 1}, "missing required name declaration",
                       "add name = \"project_name\"");
    return;
  }
  ReportDuplicates(program.names, "name", diagnostics);
  const NameDecl& decl = program.names.front();
  std::string reason;
  if (!IsValidProjectName(decl.value, &reason)) {
    diagnostics->Error(decl.value.size() > kMaxNameLength ? "E0313" : "E0312",
                       decl.value_location, "invalid name: " + reason,
                       "use letters, digits, underscores, and hyphens, "
                       "starting with a letter or underscore");
    return;
  }
  workflow->name = decl.value;
}

void ValidateSteps(const Program& program, Diagnostics* diagnostics,
                   Workflow* workflow) {
  if (program.steps.empty()) {
    // An empty file is reported through its missing headers alone.
    const bool has_any_header = !program.models.empty() ||
                                !program.backends.empty() ||
                                !program.names.empty();
    if (has_any_header) {
      diagnostics->Error("E0314", {1, 1}, "workflow has no steps",
                         "add a step block: a line `step step1():` followed "
                         "by an indented `ask(\"...\")`");
    }
    return;
  }
  if (program.steps.size() > kMaxSteps) {
    diagnostics->Error(
        "E0318", program.steps[kMaxSteps].location,
        "too many steps: the limit is " + std::to_string(kMaxSteps));
  }
  std::map<std::string, std::size_t> first_line;
  for (const StepDecl& step : program.steps) {
    const auto [it, inserted] =
        first_line.emplace(step.name, step.location.line);
    if (!inserted) {
      diagnostics->Error(
          "E0315", step.name_location,
          "duplicate step name '" + step.name + "'",
          "first declared on line " + std::to_string(it->second));
    }
    if (step.asks.size() > kMaxAsksPerStep) {
      diagnostics->Error("E0319", step.asks[kMaxAsksPerStep].location,
                         "too many ask() statements in step '" + step.name +
                             "': the limit is " +
                             std::to_string(kMaxAsksPerStep));
    }
    for (const AskStmt& ask : step.asks) {
      if (IsBlank(ask.prompt)) {
        diagnostics->Error("E0316", ask.prompt_location,
                           "ask() prompt must not be empty");
      } else if (ask.prompt.size() > kMaxPromptBytes) {
        diagnostics->Error(
            "E0317", ask.prompt_location,
            "ask() prompt is " + std::to_string(ask.prompt.size()) +
                " bytes: the limit is " + std::to_string(kMaxPromptBytes));
      }
    }
  }
  workflow->steps = program.steps;
}

}  // namespace

bool IsValidProjectName(std::string_view name, std::string* reason) {
  if (name.empty()) {
    *reason = "the name is empty";
    return false;
  }
  if (name.size() > kMaxNameLength) {
    *reason = "the name is longer than " + std::to_string(kMaxNameLength) +
              " characters";
    return false;
  }
  const char first = name.front();
  if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
        first == '_')) {
    *reason = "the name must start with a letter or underscore";
    return false;
  }
  for (const char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      *reason =
          "the name contains a character other than letters, digits, "
          "underscores, and hyphens";
      return false;
    }
  }
  return true;
}

bool ParseModelUrl(std::string_view url, ModelRef* out) {
  if (!StartsWith(url, kModelPrefix)) return false;
  std::string_view rest = url.substr(sizeof(kModelPrefix) - 1);

  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (start <= rest.size()) {
    std::size_t end = rest.find('/', start);
    if (end == std::string_view::npos) end = rest.size();
    segments.push_back(rest.substr(start, end - start));
    start = end + 1;
  }
  if (segments.size() != 2 && segments.size() != 4) return false;
  if (!IsRepoSegment(segments[0]) || !IsRepoSegment(segments[1])) return false;
  out->url = std::string(url);
  out->owner = std::string(segments[0]);
  out->repo = std::string(segments[1]);
  out->revision.clear();
  out->server.clear();
  if (segments.size() == 4) {
    if (segments[2] != "tree" || !IsRepoSegment(segments[3])) return false;
    out->revision = std::string(segments[3]);
  }
  return true;
}

bool Validate(const Program& program, Diagnostics* diagnostics,
              Workflow* workflow) {
  const std::size_t before = diagnostics->items().size();
  ValidateModel(program, diagnostics, workflow);
  ValidateBackend(program, diagnostics, workflow);
  ValidateName(program, diagnostics, workflow);
  ValidateSteps(program, diagnostics, workflow);
  return diagnostics->items().size() == before;
}

bool CheckSourceFile(const std::filesystem::path& path, SourceText* source,
                     Diagnostics* diagnostics, Workflow* workflow) {
  source->path = path.generic_string();
  std::string raw;
  std::string error;
  // The read cap is generous so that an oversized source is reported by
  // NormalizeSource as E0109 rather than as a read failure.
  if (!ReadFile(path, &raw, &error, 64u * 1024u * 1024u)) {
    diagnostics->Error("E0110", {1, 1}, error);
    return false;
  }
  return CheckSourceText(raw, path.generic_string(), source, diagnostics,
                         workflow);
}

bool CheckSourceText(std::string_view raw, std::string display_path,
                     SourceText* source, Diagnostics* diagnostics,
                     Workflow* workflow) {
  if (!NormalizeSource(raw, std::move(display_path), source, diagnostics)) {
    return false;
  }
  const std::vector<SourceLine> lines = Lex(*source, diagnostics);
  const Program program = Parse(lines, diagnostics);
  if (!diagnostics->HasErrors()) {
    return Validate(program, diagnostics, workflow);
  }
  // After a syntax error the tree is incomplete. What was parsed is still
  // validated, but "missing" and "empty" findings would only repeat the
  // syntax errors, so they are dropped.
  Diagnostics semantic;
  Validate(program, &semantic, workflow);
  for (const Diagnostic& item : semantic.items()) {
    if (item.code == "E0301" || item.code == "E0302" || item.code == "E0303" ||
        item.code == "E0314") {
      continue;
    }
    diagnostics->Error(item.code, item.location, item.message, item.hint);
  }
  return false;
}

}  // namespace seq
