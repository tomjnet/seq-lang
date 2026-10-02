// The smaller commands: new, check, doctor, model pull, and clean.

#include "cli/commands.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "project/project.hpp"
#include "seq/exit_codes.hpp"
#include "seq/model.hpp"
#include "support/config.hpp"
#include "support/process.hpp"
#include "support/util.hpp"
#include "toolchain/toolchain.hpp"
#include "validation/validator.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

int Fail(int code, const std::string& message, const std::string& hint = "") {
  std::cout << std::flush;
  std::cerr << "seqc: error: " << message << "\n";
  if (!hint.empty()) std::cerr << "  " << hint << "\n";
  return code;
}

// The project in the current directory, for commands that take no file.
bool CurrentProject(ProjectPaths* paths, std::string* error) {
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (ec || !fs::is_regular_file(cwd / "src" / "main.seq", ec)) {
    *error =
        "no Seq project here: src/main.seq was not found in the current "
        "directory";
    return false;
  }
  return ResolveProject(cwd / "src" / "main.seq", paths, error);
}

class DoctorReport {
 public:
  void Ok(const std::string& what, const std::string& detail) {
    Print("[ ok ]", what, detail);
  }
  void Warn(const std::string& what, const std::string& detail) {
    Print("[warn]", what, detail);
  }
  void Failed(const std::string& what, const std::string& detail) {
    Print("[FAIL]", what, detail);
    ++failures_;
  }
  int failures() const { return failures_; }

 private:
  static void Print(const char* mark, const std::string& what,
                    const std::string& detail) {
    std::cout << mark << " " << what << ": " << detail << "\n";
  }
  int failures_ = 0;
};

// First line of compiler output, for one-line doctor messages.
std::string FirstLine(const std::string& text) {
  const std::vector<std::string> lines = SplitLines(text);
  for (const std::string& line : lines) {
    const std::string_view trimmed = TrimWhitespace(line);
    if (!trimmed.empty()) return std::string(trimmed);
  }
  return "";
}

}  // namespace

bool UseColor() {
  if (const char* no_color = std::getenv("NO_COLOR");
      no_color != nullptr && *no_color != '\0') {
    return false;
  }
#if defined(_WIN32)
  return false;
#else
  return isatty(fileno(stderr)) != 0;
#endif
}

int CommandNew(const std::string& name,
               const std::vector<std::string>& overrides) {
  Config config;
  std::string error;
  if (!Config::Load(overrides, &config, &error)) return Fail(kExitUsage, error);
  const Installation install = LocateInstallation(config);
  std::error_code ec;
  const fs::path parent = fs::current_path(ec);
  if (ec) return Fail(kExitFilesystem, "cannot read the current directory");
  if (!ScaffoldProject(name, parent, install, std::cout, &error)) {
    std::string reason;
    if (!IsValidProjectName(name, &reason)) return Fail(kExitUsage, error);
    return Fail(kExitFilesystem, error);
  }
  return kExitOk;
}

int CommandCheck(const fs::path& source_path) {
  SourceText source;
  Diagnostics diagnostics;
  Workflow workflow;
  if (!CheckSourceFile(source_path, &source, &diagnostics, &workflow)) {
    diagnostics.Print(std::cerr, source, UseColor());
    std::cerr << "seqc: " << diagnostics.items().size() << " error(s) in "
              << source.path << "\n";
    return kExitSource;
  }
  std::size_t requests = 0;
  for (const StepDecl& step : workflow.steps) requests += step.asks.size();
  std::cout << source.path << ": ok\n"
            << "  name:    " << workflow.name << "\n"
            << "  model:   " << workflow.model.url << "\n"
            << "  backend: C\n"
            << "  steps:   " << workflow.steps.size() << " (" << requests
            << " request" << (requests == 1 ? "" : "s") << ")\n";
  for (std::size_t i = 0; i < workflow.steps.size(); ++i) {
    std::cout << "    " << (i + 1) << ". " << workflow.steps[i].name << " ("
              << workflow.steps[i].asks.size() << ")\n";
  }
  return kExitOk;
}

int CommandDoctor(const std::vector<std::string>& overrides) {
  Config config;
  std::string error;
  if (!Config::Load(overrides, &config, &error)) return Fail(kExitUsage, error);

  std::cout << "Seq Compiler doctor\n\n";
  DoctorReport report;

  const SandboxSupport sandbox = DetectSandbox();
#if defined(__linux__) && defined(__x86_64__)
  report.Ok("platform", "Linux x86_64 (" + sandbox.kernel + ")");
#else
  report.Failed("platform",
                "Seq builds and runs programs on Linux x86_64 only; this host "
                "can scaffold, check, and clean projects");
#endif
  if (sandbox.available) {
    report.Ok("sandbox", sandbox.detail);
  } else {
    report.Failed("sandbox", sandbox.detail);
  }

  const Installation install = LocateInstallation(config);
  if (HasTemplates(install, &error)) {
    report.Ok("templates", install.templates_dir.string());
  } else {
    report.Failed("templates", error);
  }
  const bool has_runtime = HasRuntime(install, &error);
  if (has_runtime) {
    report.Ok("runtime", install.home.string() +
                             " (include/seq_runtime.h, lib/libseqrt.a)");
  } else {
    report.Failed("runtime", error);
  }

  Toolchain toolchain;
  if (!LocateToolchain(config, &toolchain, &error)) {
    report.Failed("compilers", error);
  } else {
    report.Ok("C compiler",
              toolchain.cc.string() + " (" + toolchain.cc_version + ")");
    report.Ok("C++ compiler",
              toolchain.cxx.string() + " (" + toolchain.cxx_version + ")");
    if (sandbox.available) {
      // The probes run in the build sandbox, so they also prove that the
      // compiler works under it on this host.
      // A name no other seqc doctor can be using at the same time.
      std::error_code ec;
      std::random_device device;
      const fs::path scratch = fs::temp_directory_path(ec) /
                               ("seqc-doctor-" + std::to_string(device()) +
                                "-" + std::to_string(device()));
      fs::create_directories(scratch, ec);
      const CompilerDriver driver(toolchain, install, config);
      const CompileResult static_probe = driver.ProbeStaticLink(scratch);
      if (static_probe.ok) {
        report.Ok("static C library",
                  "a static program links in the build "
                  "sandbox");
      } else {
        report.Failed("static C library",
                      FirstLine(static_probe.diagnostics) +
                          " (install libc6-dev or glibc-static)");
      }
      const CompileResult gtest_probe = driver.ProbeGoogleTest(scratch);
      if (gtest_probe.ok) {
        report.Ok("Google Test",
                  toolchain.gtest_root.empty()
                      ? std::string("found on the default search path")
                      : toolchain.gtest_root.string());
      } else {
        report.Failed("Google Test", FirstLine(gtest_probe.diagnostics) +
                                         " (install libgtest-dev or set "
                                         "toolchain.gtest_root)");
      }
      fs::remove_all(scratch, ec);
    } else {
      report.Warn("static C library",
                  "not checked: the sandbox is "
                  "unavailable");
      report.Warn("Google Test", "not checked: the sandbox is unavailable");
    }
  }

  const std::string adapter = config.Get("model.adapter");
  if (adapter == "fake") {
    report.Warn("inference runtime",
                "the fake adapter is selected; it replays scripted responses "
                "and is meant for tests");
  } else if (!config.Get("model.endpoint").empty()) {
    report.Ok("inference runtime",
              "using the running server at " + config.Get("model.endpoint"));
  } else if (const auto server = FindProgram(config.Get("model.llama_server"));
             server.has_value()) {
    report.Ok("inference runtime", server->string());
  } else {
    report.Failed("inference runtime",
                  "'" + config.Get("model.llama_server") +
                      "' was not found; install llama.cpp or set "
                      "model.llama_server or model.endpoint");
  }

  ProjectPaths paths;
  if (!CurrentProject(&paths, &error)) {
    report.Warn("model",
                "not checked: the current directory is not a Seq "
                "project");
  } else if (adapter == "fake") {
    report.Warn("model", "not checked: the fake adapter needs no weights");
  } else {
    std::error_code ec;
    ModelLock lock;
    if (!fs::exists(paths.lock_file, ec)) {
      report.Failed("model",
                    "not locked for this project; run `seqc model "
                    "pull`");
    } else if (!ReadModelLock(paths.lock_file, &lock, &error)) {
      report.Failed("model", error);
    } else if (!fs::is_regular_file(ModelWeightsPath(lock), ec)) {
      report.Failed("model", "locked to " + lock.file +
                                 " but the weights are not downloaded; run "
                                 "`seqc model pull`");
    } else {
      report.Ok("model", lock.repo + " / " + lock.file + " at " +
                             lock.revision.substr(0, 12));
    }
  }

  if (const auto curl = FindProgram("curl"); curl.has_value()) {
    report.Ok("curl", curl->string() + " (used only by `seqc model pull`)");
  } else {
    report.Warn("curl", "not found; `seqc model pull` needs it");
  }

  std::cout << "\n";
  if (report.failures() == 0) {
    std::cout << "Execution is possible on this host.\n";
    return kExitOk;
  }
  std::cout << "Execution is not possible on this host yet: "
            << report.failures() << " item(s) marked FAIL need attention.\n";
  return kExitPrerequisite;
}

int CommandModelPull(bool update, const std::vector<std::string>& overrides) {
  Config config;
  std::string error;
  if (!Config::Load(overrides, &config, &error)) return Fail(kExitUsage, error);
  ProjectPaths paths;
  if (!CurrentProject(&paths, &error)) return Fail(kExitFilesystem, error);

  SourceText source;
  Diagnostics diagnostics;
  Workflow workflow;
  if (!CheckSourceFile(fs::path("src") / "main.seq", &source, &diagnostics,
                       &workflow)) {
    diagnostics.Print(std::cerr, source, UseColor());
    return kExitSource;
  }
  return ModelPull(workflow.model, config, paths.lock_file, update, std::cout,
                   std::cerr);
}

int CommandClean(bool all) {
  ProjectPaths paths;
  std::string error;
  if (!CurrentProject(&paths, &error)) return Fail(kExitFilesystem, error);

  std::error_code ec;
  FileLock busy_lock;
  if (fs::is_directory(paths.temp, ec)) {
    bool busy = false;
    if (!busy_lock.TryAcquire(paths.busy_lock, &busy, &error)) {
      if (busy) {
        return Fail(kExitFilesystem,
                    "another seqc is building or running this project");
      }
      return Fail(kExitFilesystem, error);
    }
  }
  if (!CleanProject(paths, all, std::cout, &error)) {
    return Fail(kExitFilesystem, error);
  }
  std::cout << "Clean complete. Published outputs and input/ were not "
               "touched.\n";
  return kExitOk;
}

}  // namespace seq
