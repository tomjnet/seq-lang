#ifndef SEQ_VALIDATION_VALIDATOR_HPP_
#define SEQ_VALIDATION_VALIDATOR_HPP_

#include <filesystem>
#include <string>
#include <string_view>

#include "seq/ast.hpp"
#include "seq/diagnostics.hpp"

namespace seq {

// Checks header cardinality, backend support, names, the model URL, prompts,
// and limits. Fills *workflow and returns true only when no error was found.
bool Validate(const Program& program, Diagnostics* diagnostics,
              Workflow* workflow);

// True if `name` is a safe artifact basename: ASCII letters, digits,
// underscores, and hyphens, starting with a letter or underscore, at most
// kMaxNameLength characters. On failure *reason says why.
bool IsValidProjectName(std::string_view name, std::string* reason);

// Splits https://huggingface.co/<owner>/<repo>[/tree/<revision>]. Returns
// false if `url` has any other form.
bool ParseModelUrl(std::string_view url, ModelRef* out);

// Normalizes, lexes, parses, and validates source bytes. Diagnostics are left
// in *diagnostics and the normalized text in *source for printing.
bool CheckSourceText(std::string_view raw, std::string display_path,
                     SourceText* source, Diagnostics* diagnostics,
                     Workflow* workflow);

// The same, reading the bytes from a file.
bool CheckSourceFile(const std::filesystem::path& path, SourceText* source,
                     Diagnostics* diagnostics, Workflow* workflow);

}  // namespace seq

#endif  // SEQ_VALIDATION_VALIDATOR_HPP_
