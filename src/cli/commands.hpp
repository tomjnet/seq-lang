#ifndef SEQ_CLI_COMMANDS_HPP_
#define SEQ_CLI_COMMANDS_HPP_

#include <filesystem>
#include <string>
#include <vector>

namespace seq {

struct BuildOptions {
  std::filesystem::path source;
  // Stop after a successful build so the artifacts can be inspected.
  bool build_only = false;
  // Ignore the build cache and synthesize again.
  bool rebuild = false;
  // Replace colliding outputs that the rerun rule would refuse.
  bool force = false;
  // Print only errors, program output, and the final artifact list.
  bool quiet = false;
  // --set key=value overrides, highest precedence.
  std::vector<std::string> overrides;
};

// Each command returns the process exit status (seq/exit_codes.hpp).

// seqc <file.seq>: validate, build or reuse the cached build, execute, and
// report artifacts.
int CommandBuild(const BuildOptions& options);

// seqc new <name>
int CommandNew(const std::string& name,
               const std::vector<std::string>& overrides);

// seqc check <file.seq>: parse and validate only. No model, no compiler.
int CommandCheck(const std::filesystem::path& source);

// seqc doctor
int CommandDoctor(const std::vector<std::string>& overrides);

// seqc model pull [--update], for the project in the current directory.
int CommandModelPull(bool update, const std::vector<std::string>& overrides);

// seqc clean [--all], for the project in the current directory.
int CommandClean(bool all);

// True when diagnostics should use color: stderr is a terminal and NO_COLOR
// is not set.
bool UseColor();

}  // namespace seq

#endif  // SEQ_CLI_COMMANDS_HPP_
