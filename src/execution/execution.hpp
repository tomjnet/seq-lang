#ifndef SEQ_EXECUTION_EXECUTION_HPP_
#define SEQ_EXECUTION_EXECUTION_HPP_

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

#include "planner/planner.hpp"
#include "project/project.hpp"
#include "support/config.hpp"
#include "support/process.hpp"

namespace seq {

// One step as reported by the runtime driver on descriptor 3.
struct StepRecord {
  int index = 0;
  std::string name;
  // "running" (no completion record arrived), "ok", or "failed".
  std::string status;
  int code = 0;
  std::string message;
};

struct TestCaseResult {
  std::string name;
  bool passed = false;
};

// Runs model-written code (the workflow executable or its test binary) in the
// run sandbox: it may read its own executable and input/, write only inside
// `staging`, and has no network and no ability to create processes. The
// environment holds only LC_ALL=C and TZ=UTC, stdin is /dev/null, and there
// are no arguments.
ProcessResult RunIsolated(const std::filesystem::path& binary,
                          const std::filesystem::path& staging,
                          const std::filesystem::path& input_dir,
                          const Config& config, std::ostream* live_stdout,
                          std::ostream* live_stderr);

// The filesystem view RunIsolated grants: read and execute the binary itself,
// read input/, and read and write staging. Nothing else on the machine.
SandboxPolicy RunSandboxPolicy(const std::filesystem::path& binary,
                               const std::filesystem::path& staging,
                               const std::filesystem::path& input_dir);

std::vector<StepRecord> ParseStepRecords(const std::string& report);

// Reads per-test results from Google Test's console output.
std::vector<TestCaseResult> ParseGoogleTestOutput(const std::string& output);

// One sentence on how a run ended abnormally, or an empty string if the
// process ran to completion (whatever its exit status).
std::string DescribeAbnormalEnd(const ProcessResult& result,
                                const Config& config);

struct PublishedFile {
  std::string path;
  std::uintmax_t size = 0;
  std::string sha256;
  std::string kind;
  bool final_artifact = false;
};

struct PublishOutcome {
  bool ok = false;
  // True when the failure is a refused collision or another filesystem rule,
  // rather than a problem with what the program produced.
  bool filesystem_failure = false;
  std::string error;
  std::vector<PublishedFile> published;
  // Files the program left in staging that the plan did not declare. They
  // stay in the run directory and are not published.
  std::vector<std::string> undeclared;
};

// Validates the declared outputs in `staging` and moves them into output/.
// Nothing is published unless every declared output is valid and every
// collision is allowed by the rerun rule (or `force`).
PublishOutcome ValidateAndPublish(const Plan& plan,
                                  const std::filesystem::path& staging,
                                  const ProjectPaths& paths, bool force,
                                  const Config& config);

}  // namespace seq

#endif  // SEQ_EXECUTION_EXECUTION_HPP_
