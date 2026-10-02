#ifndef SEQ_AST_HPP_
#define SEQ_AST_HPP_

#include <cstddef>
#include <string>
#include <vector>

#include "seq/diagnostics.hpp"

namespace seq {

// Language limits (docs/language.md). Exceeding one is a validation error,
// never silent truncation.
inline constexpr std::size_t kMaxSourceBytes = 1024 * 1024;
inline constexpr std::size_t kMaxNameLength = 64;
inline constexpr std::size_t kMaxPromptBytes = 4096;
inline constexpr std::size_t kMaxSteps = 64;
inline constexpr std::size_t kMaxAsksPerStep = 16;

// The model URL written by `seqc new` and used as the reference model.
inline constexpr char kReferenceModelUrl[] =
    "https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct";

// --- Syntax tree, as written -------------------------------------------------
// The parser records what the file says without judging it. Cardinality,
// support, and limits are checked by the validator.

struct Argument {
  enum class Kind { kString, kOther };
  Kind kind = Kind::kString;
  // Decoded value for a string; the source spelling otherwise.
  std::string value;
  SourceLocation location;
};

struct ModelDecl {
  SourceLocation location;
  std::vector<Argument> arguments;
};

struct BackendDecl {
  SourceLocation location;
  std::string selector;
  SourceLocation selector_location;
  std::vector<Argument> arguments;
};

struct NameDecl {
  SourceLocation location;
  std::string value;
  SourceLocation value_location;
};

struct AskStmt {
  SourceLocation location;
  std::string prompt;
  SourceLocation prompt_location;
};

struct StepDecl {
  SourceLocation location;
  std::string name;
  SourceLocation name_location;
  std::vector<AskStmt> asks;
};

struct Program {
  std::vector<ModelDecl> models;
  std::vector<BackendDecl> backends;
  std::vector<NameDecl> names;
  std::vector<StepDecl> steps;
};

// --- Validated workflow ------------------------------------------------------

enum class BackendKind { kC };

struct ModelRef {
  SourceLocation location;
  // The declared repository URL, exactly as written.
  std::string url;
  std::string owner;
  std::string repo;
  // Revision pinned in source with /tree/<revision>, or empty.
  std::string revision;
  // Reserved inference-server argument. Always empty in v0.1.
  std::string server;
};

struct Workflow {
  ModelRef model;
  BackendKind backend = BackendKind::kC;
  std::string name;
  // Steps in source order; each holds its requests in source order.
  std::vector<StepDecl> steps;
};

}  // namespace seq

#endif  // SEQ_AST_HPP_
