// `seqc <file.seq>`: the whole pipeline from source to published outputs.
//
//   validate -> inputs -> cache check -> plan -> step functions -> policy
//   -> gcc (bounded repair) -> assembly -> binary -> Google Test synthesis,
//   build, isolated run -> accept -> isolated execution -> validate outputs
//   -> publish
//
// Nothing is inferred or compiled before the source and inputs are valid, and
// nothing is executed unless a complete build was accepted.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include <random>
#include <system_error>

#include "cache/cache.hpp"
#include "cli/commands.hpp"
#include "execution/execution.hpp"
#include "planner/planner.hpp"
#include "project/project.hpp"
#include "seq/backend.hpp"
#include "seq/exit_codes.hpp"
#include "seq/model.hpp"
#include "seq/version.hpp"
#include "support/config.hpp"
#include "support/process.hpp"
#include "support/sha256.hpp"
#include "support/util.hpp"
#include "toolchain/toolchain.hpp"
#include "validation/validator.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

constexpr int kExitCancelled = 130;

class Console {
 public:
  explicit Console(bool quiet) : quiet_(quiet) {}

  // "[tag]      message", shown unless --quiet.
  void Stage(std::string_view tag, const std::string& message) const {
    if (quiet_) return;
    std::string label = "[" + std::string(tag) + "]";
    if (label.size() < 11) label.append(11 - label.size(), ' ');
    std::cout << label << message << "\n" << std::flush;
  }

  // Indented detail under a stage line, shown unless --quiet.
  void Detail(const std::string& text) const {
    if (quiet_) return;
    for (const std::string& line : SplitLines(text)) {
      std::cout << "           " << line << "\n";
    }
    std::cout << std::flush;
  }

  void Line(const std::string& text) const {
    if (!quiet_) std::cout << text << "\n" << std::flush;
  }

  // Shown even with --quiet.
  void Always(const std::string& text) const {
    std::cout << text << "\n" << std::flush;
  }

  bool quiet() const { return quiet_; }

 private:
  bool quiet_;
};

int Fail(int code, const std::string& message, const std::string& hint = "") {
  std::cout << std::flush;
  std::cerr << "seqc: error: " << message << "\n";
  if (!hint.empty()) std::cerr << "  " << hint << "\n";
  return code;
}

// Run identifiers sort by start time: a UTC timestamp with milliseconds,
// followed by a random suffix that keeps simultaneous runs apart.
std::string NewRunId() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const long long milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count() %
      1000;
  std::tm parts{};
#if defined(_WIN32)
  gmtime_s(&parts, &seconds);
#else
  gmtime_r(&seconds, &parts);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &parts);
  std::random_device device;
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), ".%03lldZ-%06x", milliseconds,
                device() & 0xFFFFFFU);
  return std::string(stamp) + suffix;
}

std::string Seconds(double seconds) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f s", seconds);
  return buffer;
}

// Creates a directory that only the owner can enter: run records can hold
// generated text and input excerpts.
bool MakePrivateDirectory(const fs::path& path, std::string* error) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    *error = "cannot create " + path.string() + ": " + ec.message();
    return false;
  }
  fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, ec);
  return true;
}

void WriteRecord(const fs::path& path, const std::string& data) {
  std::string ignored;
  WriteFile(path, data, &ignored);
}

// Everything one build needs, gathered before any inference.
struct Session {
  BuildOptions options;
  Console console{false};
  SourceText source;
  Workflow workflow;
  ProjectPaths paths;
  Config config;
  Installation install;
  InputManifest inputs;
  Toolchain toolchain;
  std::unique_ptr<ModelAdapter> adapter;
  std::unique_ptr<Backend> backend;
  GenerationSettings settings;
  std::string run_id;
  fs::path run_dir;
  fs::path attempts_dir;
  fs::path logs_dir;
  std::string cache_key;
  bool adapter_started = false;
  int total_attempts = 3;
};

struct ProgramBuild {
  fs::path dir;  // Holds <name>.c, <name>.s, and <name>.bin.
  std::string step_functions;
  std::string unit;
  std::string warnings;
  int attempts = 0;
};

struct TestBuild {
  fs::path dir;  // Holds <name>_test.cc and <name>_test.bin.
  int attempts = 0;
  Json results;
};

int CompileFailureCode(const CompileResult& result) {
  return result.isolation_failure ? kExitFilesystem : kExitPrerequisite;
}

// One bounded inference call. Checks the context budget first, and records
// the request and the response in the run directory.
int Infer(Session& session, const InferenceRequest& request,
          const std::string& label, std::string* content) {
  std::string error;
  if (!session.adapter_started) {
    session.console.Stage("model", "starting " + session.adapter->Name());
    if (!session.adapter->Start(&error)) return Fail(kExitModel, error);
    session.adapter_started = true;
  }

  int prompt_tokens = 0;
  if (!session.adapter->CountTokens(request.system + "\n" + request.user,
                                    &prompt_tokens, &error)) {
    return Fail(kExitModel, error);
  }
  if (prompt_tokens + session.settings.max_output_tokens >
      session.settings.context_tokens) {
    return Fail(
        kExitModel,
        "the " + request.kind + " prompt needs " +
            std::to_string(prompt_tokens) + " tokens plus " +
            std::to_string(session.settings.max_output_tokens) +
            " reserved for output, which exceeds the model window of " +
            std::to_string(session.settings.context_tokens),
        "shorten the prompts, reduce input/ or inputs.preview_bytes, or "
        "lower model.max_output_tokens; nothing is truncated silently");
  }

  WriteRecord(session.attempts_dir / (label + ".request.json"),
              Json::MakeObject()
                      .Set("kind", request.kind)
                      .Set("prompt_tokens", prompt_tokens)
                      .Set("system", request.system)
                      .Set("user", request.user)
                      .Set("schema", request.schema)
                      .Dump() +
                  "\n");

  const InferenceResult result =
      session.adapter->Complete(request, session.settings);
  WriteRecord(session.attempts_dir / (label + ".response.txt"),
              result.ok ? result.content : "error: " + result.error + "\n");
  if (CancellationRequested()) return Fail(kExitCancelled, "cancelled");
  if (!result.ok) {
    return Fail(kExitModel,
                "inference failed (" + request.kind + "): " + result.error);
  }
  if (result.content.size() > session.settings.max_response_bytes) {
    return Fail(kExitModel,
                "the " + request.kind + " response is " +
                    FormatBytes(result.content.size()) +
                    ", larger than the limit of " +
                    FormatBytes(session.settings.max_response_bytes) +
                    " (setting model.max_response_kib)");
  }
  *content = result.content;
  return kExitOk;
}

int SynthesizePlan(Session& session, Plan* plan) {
  session.console.Stage(
      "plan", "requesting a plan for " +
                  std::to_string(session.workflow.steps.size()) + " step(s)");
  std::string content;
  const int status =
      Infer(session,
            MakePlanRequest(session.workflow, session.inputs, *session.backend),
            "plan", &content);
  if (status != kExitOk) return status;

  std::string error;
  const PlanError result =
      ParsePlan(content, session.workflow,
                session.backend->AllowedDependencies(), plan, &error);
  if (result == PlanError::kUnsupportedDependency) {
    return Fail(kExitModel, error,
                "seqc links only the C library, libm, and its own runtime; "
                "it never installs packages a model asks for");
  }
  if (result != PlanError::kNone) {
    return Fail(kExitModel, "the model returned an unusable plan: " + error);
  }
  WriteRecord(session.run_dir / "plan.json", plan->ToJson().Dump() + "\n");

  std::size_t final_count = 0;
  for (const PlanOutput& output : plan->outputs) {
    if (output.final_artifact) ++final_count;
  }
  session.console.Stage(
      "plan", "accepted: " + std::to_string(plan->steps.size()) + " step(s), " +
                  std::to_string(plan->outputs.size()) +
                  " declared output(s), " + std::to_string(final_count) +
                  " final");
  return kExitOk;
}

// 1-based line of the unit on which the model-written text starts.
std::size_t FirstModelLine(const std::string& unit,
                           const std::string& model_text) {
  const std::size_t at = unit.find(model_text);
  if (at == std::string::npos) return 1;
  return 1 + static_cast<std::size_t>(
                 std::count(unit.begin(), unit.begin() + at, '\n'));
}

int SynthesizeProgram(Session& session, const Plan& plan,
                      const CompilerDriver& driver, ProgramBuild* build) {
  const std::string& name = session.workflow.name;
  const std::string source_name = name + ".c";
  const std::string assembly_name = name + ".s";
  const std::string binary_name = name + ".bin";

  std::string previous_source;
  std::string problems;
  for (int attempt = 1; attempt <= session.total_attempts; ++attempt) {
    const std::string label = "generate-" + std::to_string(attempt);
    session.console.Stage(attempt == 1 ? "generate" : "repair",
                          "attempt " + std::to_string(attempt) + " of " +
                              std::to_string(session.total_attempts));
    const InferenceRequest request =
        attempt == 1
            ? MakeGenerateRequest(session.workflow, session.inputs, plan,
                                  *session.backend)
            : MakeRepairRequest(session.workflow, session.inputs, plan,
                                *session.backend, previous_source, problems);
    std::string content;
    int status = Infer(session, request, label, &content);
    if (status != kExitOk) return status;

    std::string step_functions;
    std::string error;
    if (!ExtractSourceField(content, kStepFunctionsField, &step_functions,
                            &error)) {
      return Fail(kExitModel,
                  "the model returned an unusable response: " + error);
    }

    const fs::path work = session.attempts_dir / label;
    if (!MakePrivateDirectory(work, &error))
      return Fail(kExitFilesystem, error);
    const std::string unit =
        session.backend->ComposeUnit(session.workflow, step_functions);
    if (!WriteFile(work / source_name, unit, &error)) {
      return Fail(kExitFilesystem, error);
    }
    previous_source = step_functions;
    build->attempts = attempt;

    const std::vector<PolicyViolation> violations =
        session.backend->CheckStepFunctions(session.workflow, step_functions);
    if (!violations.empty()) {
      problems = DescribeViolations(violations, step_functions);
      WriteRecord(work / "policy.log", problems);
      session.console.Stage("policy", "rejected the generated source");
      session.console.Detail(problems);
      continue;
    }
    session.console.Stage("policy", "ok");

    const CompileResult compiled =
        driver.EmitAssembly(work, source_name, assembly_name);
    WriteRecord(work / "gcc.log",
                compiled.command + "\n" + compiled.diagnostics);
    if (compiled.infrastructure_failure) {
      return Fail(CompileFailureCode(compiled),
                  "the compiler could not run: " + compiled.diagnostics);
    }
    if (!compiled.ok) {
      session.console.Stage("compile",
                            "failed (" + Seconds(compiled.seconds) + ")");
      session.console.Detail(compiled.diagnostics);
      problems =
          "The compiler rejected the program. Line numbers refer to "
          "the complete file " +
          source_name + "; your code starts at line " +
          std::to_string(FirstModelLine(unit, step_functions)) + ".\n" +
          compiled.diagnostics;
      continue;
    }
    session.console.Stage(
        "compile", "ok (" + Seconds(compiled.seconds) + ", -O3)" +
                       (compiled.diagnostics.empty() ? "" : ", with warnings"));
    // Warnings are shown and recorded. They do not fail the build.
    if (!compiled.diagnostics.empty()) {
      session.console.Detail(compiled.diagnostics);
    }

    const CompileResult linked =
        driver.LinkProgram(work, assembly_name, binary_name);
    WriteRecord(work / "link.log", linked.command + "\n" + linked.diagnostics);
    if (linked.infrastructure_failure) {
      return Fail(CompileFailureCode(linked),
                  "the linker could not run: " + linked.diagnostics);
    }
    if (!linked.ok) {
      session.console.Stage("link", "failed");
      session.console.Detail(linked.diagnostics);
      problems =
          "The program compiled but did not link. Only the C standard "
          "library, libm, and the Seq runtime are available.\n" +
          linked.diagnostics;
      continue;
    }
    session.console.Stage("link", "ok (built from the emitted assembly)");

    build->dir = work;
    build->step_functions = step_functions;
    build->unit = unit;
    build->warnings = compiled.diagnostics;
    return kExitOk;
  }

  return Fail(kExitCompile,
              "the generated program was not accepted after " +
                  std::to_string(session.total_attempts) +
                  " attempt(s) (1 generation and " +
                  std::to_string(session.total_attempts - 1) + " repair(s))",
              "the attempts are kept in " +
                  GenericRelative(session.attempts_dir, session.paths.root) +
                  "; the last accepted build, if any, was left untouched");
}

int SynthesizeTests(Session& session, const Plan& plan,
                    const CompilerDriver& driver, const ProgramBuild& program,
                    TestBuild* tests) {
  const std::string& name = session.workflow.name;
  const std::string source_name = name + ".c";
  const std::string object_name = name + ".o";
  const std::string test_name = name + "_test.cc";
  const std::string binary_name = name + "_test.bin";

  std::string previous_tests;
  std::string problems;
  for (int attempt = 1; attempt <= session.total_attempts; ++attempt) {
    const std::string label = "test-" + std::to_string(attempt);
    session.console.Stage(
        "test",
        std::string(attempt == 1 ? "generating tests" : "repairing tests") +
            ", attempt " + std::to_string(attempt) + " of " +
            std::to_string(session.total_attempts));
    const InferenceRequest request =
        attempt == 1 ? MakeTestRequest(session.workflow, plan, *session.backend,
                                       program.step_functions)
                     : MakeTestRepairRequest(
                           session.workflow, plan, *session.backend,
                           program.step_functions, previous_tests, problems);
    std::string content;
    int status = Infer(session, request, label, &content);
    if (status != kExitOk) return status;

    std::string test_cases;
    std::string error;
    if (!ExtractSourceField(content, kTestCasesField, &test_cases, &error)) {
      return Fail(kExitModel,
                  "the model returned an unusable response: " + error);
    }

    const fs::path work = session.attempts_dir / label;
    if (!MakePrivateDirectory(work, &error))
      return Fail(kExitFilesystem, error);
    if (!WriteFile(work / source_name, program.unit, &error) ||
        !WriteFile(work / test_name,
                   session.backend->ComposeTest(session.workflow, test_cases),
                   &error)) {
      return Fail(kExitFilesystem, error);
    }
    previous_tests = test_cases;
    tests->attempts = attempt;

    const std::vector<PolicyViolation> violations =
        session.backend->CheckTestCases(test_cases);
    if (!violations.empty()) {
      problems = DescribeViolations(violations, test_cases);
      WriteRecord(work / "policy.log", problems);
      session.console.Stage("policy", "rejected the generated tests");
      session.console.Detail(problems);
      continue;
    }

    // The accepted program, compiled without its main() so that gtest_main
    // can supply one.
    const CompileResult object =
        driver.CompileTestObject(work, source_name, object_name);
    if (!object.ok) {
      return Fail(object.infrastructure_failure ? CompileFailureCode(object)
                                                : kExitInternal,
                  "the accepted program could not be compiled for testing: " +
                      object.diagnostics);
    }
    const CompileResult linked =
        driver.LinkTest(work, test_name, object_name, binary_name);
    WriteRecord(work / "g++.log", linked.command + "\n" + linked.diagnostics);
    if (linked.infrastructure_failure) {
      return Fail(CompileFailureCode(linked),
                  "the C++ compiler could not run: " + linked.diagnostics);
    }
    if (!linked.ok) {
      session.console.Stage("test", "the test file did not compile (" +
                                        Seconds(linked.seconds) + ")");
      session.console.Detail(linked.diagnostics);
      problems =
          "The test file did not compile or link.\n" + linked.diagnostics;
      continue;
    }
    session.console.Stage(
        "test", "built " + binary_name + " (" + Seconds(linked.seconds) + ")");
    tests->dir = work;
    return kExitOk;
  }

  return Fail(kExitCompile,
              "the generated tests were not accepted after " +
                  std::to_string(session.total_attempts) +
                  " attempt(s); a build without a compiled test file is not "
                  "accepted",
              "the attempts are kept in " +
                  GenericRelative(session.attempts_dir, session.paths.root));
}

// Runs the generated tests in their own staging directory, so their file
// effects never mix with the real run. Failing cases are reported and
// recorded; they do not block the build.
int RunGeneratedTests(Session& session, TestBuild* tests) {
  const fs::path staging = session.run_dir / "test-staging";
  std::string error;
  if (!MakePrivateDirectory(staging, &error)) {
    return Fail(kExitFilesystem, error);
  }
  const fs::path binary = tests->dir / (session.workflow.name + "_test.bin");
  const ProcessResult result = RunIsolated(binary, staging, session.paths.input,
                                           session.config, nullptr, nullptr);
  WriteRecord(session.logs_dir / "test-stdout.log", result.out);
  WriteRecord(session.logs_dir / "test-stderr.log", result.err);
  if (!result.launched && result.isolation_failure) {
    return Fail(kExitFilesystem,
                "isolation failed for the test binary: " + result.launch_error,
                "seqc does not run generated code without isolation");
  }

  const std::vector<TestCaseResult> cases = ParseGoogleTestOutput(result.out);
  std::size_t passed = 0;
  Json list = Json::MakeArray();
  for (const TestCaseResult& item : cases) {
    if (item.passed) ++passed;
    list.Push(
        Json::MakeObject().Set("name", item.name).Set("passed", item.passed));
    session.console.Stage(
        "test", item.name + ": " + (item.passed ? "passed" : "FAILED"));
  }
  const std::string abnormal = DescribeAbnormalEnd(result, session.config);
  tests->results =
      Json::MakeObject()
          .Set("status", abnormal.empty() ? "completed" : "abnormal")
          .Set("detail", abnormal)
          .Set("exit_code", result.exit_code)
          .Set("passed", passed)
          .Set("failed", cases.size() - passed)
          .Set("cases", std::move(list));
  if (!abnormal.empty()) {
    session.console.Stage("test",
                          "the test binary did not finish: " + abnormal);
  }
  session.console.Stage(
      "test", std::to_string(passed) + " passed, " +
                  std::to_string(cases.size() - passed) +
                  " failed (model-written tests are reported, not enforced)");
  return kExitOk;
}

// Replaces the top-level artifact set with a new accepted build. build.json
// is removed first and written last: it is the marker of a complete set, so
// an interrupted replacement can never be mistaken for an accepted build.
int AcceptBuild(Session& session, const Plan& plan, const ProgramBuild& program,
                const TestBuild& tests, Json* manifest) {
  const std::string& name = session.workflow.name;
  const ArtifactPaths artifacts = ArtifactPathsFor(session.paths, name);
  const std::pair<fs::path, fs::path> moves[] = {
      {program.dir / (name + ".c"), artifacts.source},
      {program.dir / (name + ".s"), artifacts.assembly},
      {program.dir / (name + ".bin"), artifacts.binary},
      {tests.dir / (name + "_test.cc"), artifacts.test_source},
      {tests.dir / (name + "_test.bin"), artifacts.test_binary},
  };

  std::error_code ec;
  // Stage every copy next to its destination before replacing anything.
  for (const auto& [from, to] : moves) {
    fs::path staged = to;
    staged += ".new";
    fs::copy_file(from, staged, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      return Fail(kExitFilesystem,
                  "cannot stage " + to.string() + ": " + ec.message());
    }
  }
  fs::remove(session.paths.build_file, ec);
  for (const auto& [from, to] : moves) {
    fs::path staged = to;
    staged += ".new";
    fs::rename(staged, to, ec);
    if (ec) {
      return Fail(kExitFilesystem,
                  "cannot replace " + to.string() + ": " + ec.message());
    }
  }

  Json hashes = Json::MakeObject();
  const std::pair<const char*, fs::path> hashed[] = {
      {"source", artifacts.source},
      {"assembly", artifacts.assembly},
      {"binary", artifacts.binary},
      {"test_source", artifacts.test_source},
      {"test_binary", artifacts.test_binary},
  };
  for (const auto& [key, path] : hashed) {
    std::string hash;
    std::string error;
    if (!Sha256File(path, &hash, &error)) return Fail(kExitFilesystem, error);
    hashes.Set(key, hash);
  }

  *manifest = Json::MakeObject();
  manifest->Set("schema_version", 1);
  manifest->Set("cache_key", session.cache_key);
  manifest->Set("created", TimestampUtc());
  manifest->Set("run_id", session.run_id);
  manifest->Set("workflow", name);
  manifest->Set("seqc_version", kVersion);
  manifest->Set("seqc_commit", kGitCommit);
  manifest->Set("grammar_version", kGrammarVersion);
  manifest->Set("prompt_version", kPromptVersion);
  manifest->Set("source_sha256", Sha256Hex(session.source.text));
  manifest->Set("inputs", session.inputs.Identity());
  manifest->Set("model", session.adapter->Identity());
  manifest->Set("generation", session.settings.ToJson());
  manifest->Set("toolchain", session.toolchain.ToJson());
  manifest->Set("attempts", Json::MakeObject()
                                .Set("program", program.attempts)
                                .Set("tests", tests.attempts)
                                .Set("limit", session.total_attempts));
  manifest->Set("warnings", program.warnings);
  manifest->Set("plan", plan.ToJson());
  manifest->Set("tests", tests.results);
  manifest->Set("artifacts", std::move(hashes));

  std::string error;
  const std::string text = manifest->Dump() + "\n";
  if (!WriteFile(session.paths.build_file, text, &error)) {
    return Fail(kExitFilesystem, error);
  }
  WriteRecord(session.run_dir / "build-manifest.json", text);
  session.console.Stage("build", "accepted");
  return kExitOk;
}

void ReportArtifacts(const Session& session) {
  const ArtifactPaths artifacts =
      ArtifactPathsFor(session.paths, session.workflow.name);
  session.console.Line("\nBuild artifacts:");
  for (const fs::path& path :
       {artifacts.source, artifacts.assembly, artifacts.binary,
        artifacts.test_source, artifacts.test_binary}) {
    session.console.Line("  " + GenericRelative(path, session.paths.root));
  }
  session.console.Line("Run records:\n  " +
                       GenericRelative(session.run_dir, session.paths.root) +
                       "/");
}

int Execute(Session& session, const Plan& plan, bool cached) {
  const std::string& name = session.workflow.name;
  const ArtifactPaths artifacts = ArtifactPathsFor(session.paths, name);
  const fs::path staging = session.run_dir / "staging";
  std::string error;
  if (!MakePrivateDirectory(staging, &error)) {
    return Fail(kExitFilesystem, error);
  }

  session.console.Stage("run",
                        GenericRelative(artifacts.binary, session.paths.root) +
                            " (isolated: no network, no processes, "
                            "writes only to staging)");
  session.console.Line("---- program output ----");
  const ProcessResult result =
      RunIsolated(artifacts.binary, staging, session.paths.input,
                  session.config, &std::cout, &std::cerr);
  std::cout << std::flush;
  std::cerr << std::flush;
  if (!result.out.empty() && result.out.back() != '\n') {
    session.console.Line("");
  }
  session.console.Line("---- end of program output ----");

  WriteRecord(session.logs_dir / "stdout.log", result.out);
  WriteRecord(session.logs_dir / "stderr.log", result.err);
  WriteRecord(session.logs_dir / "steps.log", result.report);

  const std::vector<StepRecord> steps = ParseStepRecords(result.report);
  Json step_list = Json::MakeArray();
  const StepRecord* stopped_at = nullptr;
  for (const StepRecord& step : steps) {
    step_list.Push(Json::MakeObject()
                       .Set("index", step.index)
                       .Set("name", step.name)
                       .Set("status", step.status)
                       .Set("code", step.code)
                       .Set("message", step.message));
    if (step.status != "ok" && stopped_at == nullptr) stopped_at = &step;
    session.console.Stage("run", "step " + std::to_string(step.index) + " (" +
                                     step.name + "): " + step.status);
  }

  Json manifest = Json::MakeObject();
  manifest.Set("schema_version", 1);
  manifest.Set("run_id", session.run_id);
  manifest.Set("finished", TimestampUtc());
  manifest.Set("cache_key", session.cache_key);
  manifest.Set("reused_cached_build", cached);
  manifest.Set("exit_code", result.exit_code);
  manifest.Set("signal", result.signal);
  manifest.Set("seconds", result.seconds);
  manifest.Set("steps", std::move(step_list));
  const auto finish = [&](const std::string& status, int code) {
    manifest.Set("status", status);
    WriteRecord(session.run_dir / "run-manifest.json", manifest.Dump() + "\n");
    return code;
  };

  if (!result.launched) {
    if (result.isolation_failure) {
      return finish(
          "isolation-failure",
          Fail(kExitFilesystem, "isolation failed: " + result.launch_error,
               "seqc does not run generated code without isolation"));
    }
    return finish("not-started",
                  Fail(kExitExecution, "the program could not be started: " +
                                           result.launch_error));
  }

  const std::string step_text =
      stopped_at == nullptr ? std::string()
                            : " in step " + std::to_string(stopped_at->index) +
                                  " (" + stopped_at->name + ")";
  const std::string abnormal = DescribeAbnormalEnd(result, session.config);
  if (!abnormal.empty()) {
    return finish(
        "abnormal-end",
        Fail(result.cancelled ? kExitCancelled : kExitExecution,
             "the program did not finish" + step_text + ": " + abnormal,
             "nothing was published"));
  }
  if (result.exit_code != 0) {
    std::string message = "the program failed" + step_text;
    if (stopped_at != nullptr && stopped_at->status == "failed") {
      message = "step " + std::to_string(stopped_at->index) + " (" +
                stopped_at->name + ") failed with code " +
                std::to_string(stopped_at->code);
      if (!stopped_at->message.empty()) message += ": " + stopped_at->message;
    } else {
      message += " with exit status " + std::to_string(result.exit_code);
    }
    return finish("step-failed",
                  Fail(kExitExecution, message,
                       "later steps were not run and nothing was published"));
  }

  const PublishOutcome published = ValidateAndPublish(
      plan, staging, session.paths, session.options.force, session.config);
  Json undeclared = Json::MakeArray();
  for (const std::string& path : published.undeclared) undeclared.Push(path);
  manifest.Set("undeclared", std::move(undeclared));
  if (!published.ok) {
    manifest.Set("published", Json::MakeArray());
    return finish(
        published.filesystem_failure ? "publish-refused" : "invalid-outputs",
        Fail(published.filesystem_failure ? kExitFilesystem : kExitExecution,
             published.error, "nothing was published"));
  }
  Json published_list = Json::MakeArray();
  for (const PublishedFile& file : published.published) {
    published_list.Push(Json::MakeObject()
                            .Set("path", file.path)
                            .Set("size", file.size)
                            .Set("sha256", file.sha256)
                            .Set("kind", file.kind)
                            .Set("final", file.final_artifact));
  }
  manifest.Set("published", std::move(published_list));
  finish("ok", kExitOk);

  for (const std::string& path : published.undeclared) {
    session.console.Stage(
        "publish", "not published (the plan did not declare it): " + path +
                       ", kept in " +
                       GenericRelative(staging, session.paths.root));
  }

  // The artifact list is shown even with --quiet.
  if (published.published.empty()) {
    session.console.Always(
        "\nResults: none (the workflow printed its result "
        "to the console)");
  } else {
    session.console.Always("\nResults:");
    for (const PublishedFile& file : published.published) {
      session.console.Always("  output/" + file.path + "  (" +
                             FormatBytes(file.size) + ")" +
                             (file.final_artifact ? "  final" : ""));
    }
  }
  ReportArtifacts(session);
  return kExitOk;
}

// Everything after source validation. Only reachable on Linux x86_64.
[[maybe_unused]] int BuildAndRun(Session& session) {
  const BuildOptions& options = session.options;
  std::string error;
  if (!ResolveProject(options.source, &session.paths, &error)) {
    return Fail(kExitFilesystem, error);
  }
  if (!Config::Load(options.overrides, &session.config, &error)) {
    return Fail(kExitUsage, error);
  }
  session.install = LocateInstallation(session.config);
  if (!HasTemplates(session.install, &error) ||
      !HasRuntime(session.install, &error)) {
    return Fail(kExitPrerequisite, error);
  }
  if (!EnsureProjectLayout(session.paths, session.install, &error)) {
    return Fail(kExitFilesystem, error);
  }

  // One build or run per project at a time.
  FileLock busy_lock;
  bool busy = false;
  if (!busy_lock.TryAcquire(session.paths.busy_lock, &busy, &error)) {
    if (busy) {
      return Fail(kExitFilesystem,
                  "another seqc is already building or running this project",
                  "wait for it to finish, then run the command again");
    }
    return Fail(kExitFilesystem, error);
  }

  // Inputs.
  if (!EnumerateInputs(session.paths, session.config, &session.inputs,
                       &error)) {
    return Fail(kExitFilesystem, error);
  }
  session.console.Stage("inputs", std::to_string(session.inputs.files.size()) +
                                      " file(s), " +
                                      FormatBytes(session.inputs.total_bytes));

  // Isolation is required for compiling and for running. There is no
  // fallback to unrestricted execution.
  const SandboxSupport sandbox = DetectSandbox();
  if (!sandbox.available) {
    return Fail(kExitFilesystem, "isolation is unavailable: " + sandbox.detail,
                "seqc does not compile or run generated code without "
                "isolation; run `seqc doctor` for details");
  }

  if (!LocateToolchain(session.config, &session.toolchain, &error)) {
    return Fail(kExitPrerequisite, error);
  }

  // Run directory and model adapter.
  session.run_id = NewRunId();
  session.run_dir = session.paths.runs / session.run_id;
  session.attempts_dir = session.run_dir / "attempts";
  session.logs_dir = session.run_dir / "logs";
  if (!MakePrivateDirectory(session.run_dir, &error) ||
      !MakePrivateDirectory(session.attempts_dir, &error) ||
      !MakePrivateDirectory(session.logs_dir, &error)) {
    return Fail(kExitFilesystem, error);
  }
  WriteRecord(session.run_dir / "input-manifest.json",
              session.inputs.ToJson().Dump() + "\n");

  session.adapter =
      CreateModelAdapter(session.config, session.workflow,
                         session.paths.lock_file, session.logs_dir, &error);
  if (session.adapter == nullptr) return Fail(kExitModel, error);
  session.settings = GenerationSettings::FromConfig(session.config);
  session.total_attempts =
      1 + static_cast<int>(session.config.GetInt("build.repair_attempts"));

  std::string runtime_header;
  if (!ReadFile(session.install.include_dir / "seq_runtime.h", &runtime_header,
                &error)) {
    return Fail(kExitPrerequisite, error);
  }
  std::string runtime_library_hash;
  if (!Sha256File(session.install.lib_dir / "libseqrt.a", &runtime_library_hash,
                  &error)) {
    return Fail(kExitPrerequisite, error);
  }
  session.backend = CreateBackend(session.workflow.backend, runtime_header);

  // Build cache.
  session.cache_key = ComputeCacheKey(
      Json::MakeObject()
          .Set("seqc_version", kVersion)
          .Set("grammar_version", kGrammarVersion)
          .Set("prompt_version", kPromptVersion)
          .Set("source_sha256", Sha256Hex(session.source.text))
          .Set("inputs", session.inputs.Identity())
          .Set("model", session.adapter->Identity())
          .Set("generation", session.settings.ToJson())
          .Set("repair_attempts", session.total_attempts - 1)
          .Set("runtime_header_sha256", Sha256Hex(runtime_header))
          .Set("runtime_library_sha256", runtime_library_hash)
          .Set("toolchain", session.toolchain.ToJson()));

  Plan plan;
  Json manifest;
  bool cached = false;
  std::string miss_reason = "--rebuild was given";
  if (!options.rebuild &&
      LoadAcceptedBuild(session.paths, session.workflow.name, session.cache_key,
                        &manifest, &plan, &miss_reason)) {
    cached = true;
    session.console.Stage("cache", "reusing the accepted build " +
                                       session.cache_key.substr(0, 12) +
                                       " (no inference, no compilation)");
    if (const Json* tests = manifest.Find("tests"); tests != nullptr) {
      session.console.Stage(
          "test", "recorded results: " +
                      std::to_string(tests->GetInt("passed")) + " passed, " +
                      std::to_string(tests->GetInt("failed")) + " failed");
    }
    WriteRecord(session.run_dir / "plan.json", plan.ToJson().Dump() + "\n");
  } else {
    session.console.Stage("cache", "building: " + miss_reason);

    const CompilerDriver driver(session.toolchain, session.install,
                                session.config);
    // Prerequisites are checked before inference so that a missing library
    // is reported as such, not as a generated-code error.
    const fs::path probe_dir = session.attempts_dir / "probe";
    if (!MakePrivateDirectory(probe_dir, &error)) {
      return Fail(kExitFilesystem, error);
    }
    const CompileResult static_probe = driver.ProbeStaticLink(probe_dir);
    if (static_probe.isolation_failure) {
      return Fail(kExitFilesystem,
                  "isolation failed: " + static_probe.diagnostics);
    }
    if (!static_probe.ok) {
      return Fail(kExitPrerequisite,
                  "a static C program does not link: " +
                      std::string(TrimWhitespace(static_probe.diagnostics)),
                  "install the static C library (libc6-dev on Debian and "
                  "Ubuntu, glibc-static on Fedora)");
    }
    const CompileResult gtest_probe = driver.ProbeGoogleTest(probe_dir);
    if (!gtest_probe.ok) {
      return Fail(kExitPrerequisite,
                  "Google Test is not usable: " +
                      std::string(TrimWhitespace(gtest_probe.diagnostics)),
                  "install it (libgtest-dev on Debian and Ubuntu) or set "
                  "toolchain.gtest_root");
    }
    std::error_code ec;
    fs::remove_all(probe_dir, ec);

    int status = SynthesizePlan(session, &plan);
    if (status != kExitOk) return status;

    ProgramBuild program;
    status = SynthesizeProgram(session, plan, driver, &program);
    if (status != kExitOk) return status;

    TestBuild tests;
    status = SynthesizeTests(session, plan, driver, program, &tests);
    if (status != kExitOk) return status;
    status = RunGeneratedTests(session, &tests);
    if (status != kExitOk) return status;

    status = AcceptBuild(session, plan, program, tests, &manifest);
    if (status != kExitOk) return status;
  }
  // The inference server is not needed past this point.
  session.adapter.reset();

  if (options.build_only) {
    session.console.Stage("build", "stopping before execution (--build-only)");
    ReportArtifacts(session);
    return kExitOk;
  }
  if (CancellationRequested()) return Fail(kExitCancelled, "cancelled");
  return Execute(session, plan, cached);
}

}  // namespace

int CommandBuild(const BuildOptions& options) {
  Session session;
  session.options = options;
  session.console = Console(options.quiet);
  session.console.Line("Seq Compiler\n");

  // The workflow must be valid before anything else happens.
  Diagnostics diagnostics;
  if (!CheckSourceFile(options.source, &session.source, &diagnostics,
                       &session.workflow)) {
    diagnostics.Print(std::cerr, session.source, UseColor());
    std::cerr << "seqc: " << diagnostics.items().size() << " error(s) in "
              << session.source.path << "; nothing was built\n";
    return kExitSource;
  }
  std::size_t request_count = 0;
  for (const StepDecl& step : session.workflow.steps) {
    request_count += step.asks.size();
  }
  session.console.Stage("check",
                        session.source.path + ": " +
                            std::to_string(session.workflow.steps.size()) +
                            " step(s), " + std::to_string(request_count) +
                            " request(s), backend C");

#if defined(__linux__) && defined(__x86_64__)
  return BuildAndRun(session);
#else
  return Fail(kExitPrerequisite,
              "building and running workflows is only supported on Linux "
              "x86_64",
              "on Windows, use seqc inside WSL2 or a Linux VM; this build of "
              "seqc can scaffold, check, and clean projects");
#endif
}

}  // namespace seq
