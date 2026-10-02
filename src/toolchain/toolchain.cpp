#include "toolchain/toolchain.hpp"

#include <system_error>

#include "support/util.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

bool ReadVersion(const fs::path& program, std::string* version,
                 std::string* error) {
  ProcessSpec spec;
  spec.argv = {program.string(), "--version"};
  spec.env = {"PATH=/usr/bin:/bin", "LC_ALL=C"};
  spec.limits.wall_seconds = 30;
  const ProcessResult result = RunProcess(spec);
  if (!result.Succeeded()) {
    *error = "cannot run " + program.string() + " --version" +
             (result.launched ? "" : ": " + result.launch_error);
    return false;
  }
  const std::vector<std::string> lines = SplitLines(result.out);
  *version = lines.empty() ? "" : lines.front();
  return true;
}

}  // namespace

Json Toolchain::ToJson() const {
  return Json::MakeObject()
      .Set("cc", cc.string())
      .Set("cc_version", cc_version)
      .Set("cxx", cxx.string())
      .Set("cxx_version", cxx_version)
      .Set("gtest_root", gtest_root.string());
}

bool LocateToolchain(const Config& config, Toolchain* toolchain,
                     std::string* error) {
  const std::string cc_name = config.Get("toolchain.cc");
  const std::optional<fs::path> cc = FindProgram(cc_name);
  if (!cc.has_value()) {
    *error = "C compiler '" + cc_name +
             "' was not found; install GCC or set toolchain.cc";
    return false;
  }
  const std::string cxx_name = config.Get("toolchain.cxx");
  const std::optional<fs::path> cxx = FindProgram(cxx_name);
  if (!cxx.has_value()) {
    *error = "C++ compiler '" + cxx_name +
             "' was not found; install g++ or set toolchain.cxx (it builds "
             "the generated tests)";
    return false;
  }
  toolchain->cc = *cc;
  toolchain->cxx = *cxx;
  if (!ReadVersion(*cc, &toolchain->cc_version, error) ||
      !ReadVersion(*cxx, &toolchain->cxx_version, error)) {
    return false;
  }
  const std::string gtest_root = config.Get("toolchain.gtest_root");
  if (!gtest_root.empty()) {
    std::error_code ec;
    toolchain->gtest_root = fs::weakly_canonical(fs::path(gtest_root), ec);
    if (ec || !fs::is_directory(toolchain->gtest_root, ec)) {
      *error = "toolchain.gtest_root is not a directory: " + gtest_root;
      return false;
    }
  }
  return true;
}

CompilerDriver::CompilerDriver(const Toolchain& toolchain,
                               const Installation& install,
                               const Config& config)
    : toolchain_(toolchain),
      install_(install),
      timeout_seconds_(static_cast<std::uint64_t>(
          config.GetInt("build.compile_timeout_seconds"))),
      memory_bytes_(
          static_cast<std::uint64_t>(config.GetInt("build.compile_memory_mb")) *
          1024 * 1024) {}

CompileResult CompilerDriver::Run(const fs::path& work_dir,
                                  std::vector<std::string> argv) const {
  CompileResult outcome;
  outcome.command = Join(argv, " ");

  std::error_code ec;
  const fs::path tmp = work_dir / "tmp";
  fs::create_directories(tmp, ec);

  ProcessSpec spec;
  spec.argv = std::move(argv);
  spec.cwd = work_dir;
  // Nothing from the caller's environment reaches the compiler: variables
  // such as CPATH or LIBRARY_PATH could otherwise redirect it.
  spec.env = {
      "PATH=" + fs::path(spec.argv[0]).parent_path().string() +
          ":/usr/bin:/bin",
      "LC_ALL=C",
      "TMPDIR=" + tmp.string(),
  };
  spec.limits.wall_seconds = timeout_seconds_;
  spec.limits.memory_bytes = memory_bytes_;
  spec.limits.output_bytes = 4u * 1024u * 1024u;
  spec.limits.file_bytes = 512u * 1024u * 1024u;

  spec.sandbox = BuildSandboxPolicy(work_dir);

  const ProcessResult result = RunProcess(spec);
  outcome.seconds = result.seconds;
  outcome.diagnostics = result.err;
  if (!result.out.empty()) outcome.diagnostics += result.out;
  fs::remove_all(tmp, ec);

  if (!result.launched) {
    outcome.infrastructure_failure = true;
    outcome.isolation_failure = result.isolation_failure;
    outcome.diagnostics = result.launch_error;
    return outcome;
  }
  if (result.cancelled) {
    outcome.infrastructure_failure = true;
    outcome.diagnostics = "cancelled";
    return outcome;
  }
  if (result.timed_out) {
    outcome.infrastructure_failure = true;
    outcome.diagnostics = "the compiler did not finish within " +
                          std::to_string(timeout_seconds_) +
                          " seconds (setting build.compile_timeout_seconds)";
    return outcome;
  }
  if (result.output_limit_exceeded || result.signal != 0) {
    outcome.infrastructure_failure = true;
    outcome.diagnostics += result.signal != 0
                               ? "\nthe compiler was terminated by signal " +
                                     std::to_string(result.signal)
                               : "\nthe compiler produced too much output";
    return outcome;
  }
  outcome.ok = result.exit_code == 0;
  return outcome;
}

SandboxPolicy CompilerDriver::BuildSandboxPolicy(
    const fs::path& work_dir) const {
  // The compiler reads the system toolchain and the seqc runtime, and writes
  // only inside its working directory. It may start its own subprocesses but
  // has no network.
  SandboxPolicy policy;
  policy.allow_process_creation = true;
  policy.read_exec = {"/usr", "/bin", "/lib", "/lib64", "/lib32", "/libx32"};
  for (const fs::path& compiler : {toolchain_.cc, toolchain_.cxx}) {
    // A compiler installed under its own prefix, such as /opt/gcc/bin/gcc,
    // needs that prefix. A prefix of "/" would expose the whole filesystem.
    const fs::path prefix = compiler.parent_path().parent_path();
    if (prefix != prefix.root_path()) policy.read_exec.push_back(prefix);
  }
  // From /etc the toolchain needs only the dynamic linker cache and the
  // alternatives links; the rest of /etc stays out of reach of an #include.
  policy.read_only = {"/etc/ld.so.cache", "/etc/alternatives",
                      install_.include_dir, install_.lib_dir};
  if (!toolchain_.gtest_root.empty()) {
    policy.read_only.push_back(toolchain_.gtest_root);
  }
  policy.read_write = {work_dir, "/dev/null"};
  return policy;
}

CompileResult CompilerDriver::EmitAssembly(const fs::path& work_dir,
                                           const std::string& source,
                                           const std::string& assembly) const {
  return Run(work_dir, {toolchain_.cc.string(), "-std=c17", "-O3", "-Wall",
                        "-Wextra", "-Wpedantic", "-fdiagnostics-color=never",
                        "-I" + install_.include_dir.string(), "-S", source,
                        "-o", assembly});
}

CompileResult CompilerDriver::LinkProgram(const fs::path& work_dir,
                                          const std::string& assembly,
                                          const std::string& binary) const {
  // Link arguments come only from the compiler-owned allowlist: the C
  // library, libm, and the seqc runtime.
  return Run(work_dir, {toolchain_.cc.string(), "-fdiagnostics-color=never",
                        assembly, "-o", binary, "-static",
                        "-L" + install_.lib_dir.string(), "-lseqrt", "-lm"});
}

CompileResult CompilerDriver::CompileTestObject(
    const fs::path& work_dir, const std::string& source,
    const std::string& object) const {
  return Run(work_dir,
             {toolchain_.cc.string(), "-std=c17", "-O3", "-DSEQ_NO_MAIN",
              "-fdiagnostics-color=never", "-I" + install_.include_dir.string(),
              "-c", source, "-o", object});
}

CompileResult CompilerDriver::LinkTest(const fs::path& work_dir,
                                       const std::string& test_source,
                                       const std::string& object,
                                       const std::string& binary) const {
  std::vector<std::string> argv = {toolchain_.cxx.string(), "-std=c++17",
                                   "-fdiagnostics-color=never",
                                   "-I" + install_.include_dir.string()};
  if (!toolchain_.gtest_root.empty()) {
    argv.push_back("-I" + (toolchain_.gtest_root / "include").string());
  }
  argv.insert(argv.end(), {test_source, object, "-o", binary, "-static",
                           "-L" + install_.lib_dir.string()});
  if (!toolchain_.gtest_root.empty()) {
    argv.push_back("-L" + (toolchain_.gtest_root / "lib").string());
  }
  // gtest_main depends on gtest, so with static archives it must come first.
  argv.insert(argv.end(),
              {"-lseqrt", "-lgtest_main", "-lgtest", "-lpthread", "-lm"});
  return Run(work_dir, std::move(argv));
}

CompileResult CompilerDriver::ProbeStaticLink(const fs::path& work_dir) const {
  std::string error;
  if (!WriteFile(work_dir / "probe.c", "int main(void) { return 0; }\n",
                 &error)) {
    CompileResult outcome;
    outcome.infrastructure_failure = true;
    outcome.diagnostics = error;
    return outcome;
  }
  return Run(work_dir, {toolchain_.cc.string(), "-fdiagnostics-color=never",
                        "probe.c", "-o", "probe.bin", "-static"});
}

CompileResult CompilerDriver::ProbeGoogleTest(const fs::path& work_dir) const {
  std::string error;
  if (!WriteFile(work_dir / "probe_test.cc",
                 "#include <gtest/gtest.h>\n"
                 "TEST(Probe, Links) { EXPECT_EQ(1, 1); }\n",
                 &error)) {
    CompileResult outcome;
    outcome.infrastructure_failure = true;
    outcome.diagnostics = error;
    return outcome;
  }
  std::vector<std::string> argv = {toolchain_.cxx.string(), "-std=c++17",
                                   "-fdiagnostics-color=never"};
  if (!toolchain_.gtest_root.empty()) {
    argv.push_back("-I" + (toolchain_.gtest_root / "include").string());
    argv.push_back("-L" + (toolchain_.gtest_root / "lib").string());
  }
  argv.insert(argv.end(), {"probe_test.cc", "-o", "probe_test.bin", "-static",
                           "-lgtest_main", "-lgtest", "-lpthread"});
  return Run(work_dir, std::move(argv));
}

}  // namespace seq
