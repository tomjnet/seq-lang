// Denied-access tests for the sandbox. Acceptance of the isolation contract
// requires observing real denials, not warnings or keyword scans, so every
// case here runs a real process and checks what the kernel let it do.
//
// The policies are the ones seqc itself uses: RunSandboxPolicy for generated
// programs and CompilerDriver::BuildSandboxPolicy for the compiler.

#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "execution/execution.hpp"
#include "project/project.hpp"
#include "support/config.hpp"
#include "support/process.hpp"
#include "test_harness.hpp"
#include "toolchain/toolchain.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kAllowed = 0;
constexpr int kDenied = 1;

// A project-shaped directory tree with one file in each area that matters.
struct Fixture {
  fs::path root;
  fs::path input;
  fs::path staging;
  fs::path temp;     // Compiler-owned artifacts.
  fs::path outside;  // An unrelated directory elsewhere.

  Fixture() {
    static int counter = 0;
    root =
        fs::temp_directory_path() / ("seq-sandbox-" + std::to_string(getpid()) +
                                     "-" + std::to_string(counter++));
    fs::remove_all(root);
    input = root / "project" / "input";
    temp = root / "project" / "output" / "temp";
    staging = temp / "runs" / "r1" / "staging";
    outside = root / "elsewhere";
    for (const fs::path& dir : {input, staging, outside}) {
      fs::create_directories(dir);
    }
    Write(input / "data.txt", "input data\n");
    Write(temp / "demo.c", "compiler-owned source\n");
    Write(outside / "secret.txt", "unrelated host file\n");
    Write(staging / "mine.txt", "written by the program\n");
  }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  static void Write(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
  }
  static std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }
};

const fs::path kProbe = SEQ_PROBE_PATH;

seq::ProcessSpec ProbeSpec(const Fixture& fixture,
                           std::vector<std::string> args) {
  seq::ProcessSpec spec;
  spec.argv = {kProbe.string()};
  spec.argv.insert(spec.argv.end(), args.begin(), args.end());
  spec.env = {"LC_ALL=C", "TZ=UTC"};
  spec.cwd = fixture.staging;
  spec.limits.wall_seconds = 20;
  return spec;
}

// Runs the probe under the policy of a generated program.
seq::ProcessResult Sandboxed(const Fixture& fixture,
                             std::vector<std::string> args) {
  seq::ProcessSpec spec = ProbeSpec(fixture, std::move(args));
  spec.sandbox = seq::RunSandboxPolicy(kProbe, fixture.staging, fixture.input);
  return seq::RunProcess(spec);
}

// Runs the probe with no sandbox, as the control for each denial.
seq::ProcessResult Unrestricted(const Fixture& fixture,
                                std::vector<std::string> args) {
  return seq::RunProcess(ProbeSpec(fixture, std::move(args)));
}

int Exit(const seq::ProcessResult& result) {
  if (!result.launched) {
    std::fprintf(stderr, "  not launched: %s\n", result.launch_error.c_str());
    return -100;
  }
  if (result.signal != 0) return -result.signal;
  return result.exit_code;
}

// Checks that the action works without the sandbox and is denied inside it.
void ExpectDeniedOnlyInSandbox(const Fixture& fixture,
                               const std::vector<std::string>& args,
                               const char* file, int line) {
  const int outside = Exit(Unrestricted(fixture, args));
  const int inside = Exit(Sandboxed(fixture, args));
  if (outside == kAllowed && inside == kDenied) return;
  std::string what;
  for (const std::string& arg : args) what += arg + " ";
  seqtest::ReportFailure(file, line,
                         what +
                             ": expected allowed (0) without the sandbox "
                             "and denied (1) inside it, got " +
                             std::to_string(outside) + " and " +
                             std::to_string(inside));
}

#define EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, ...) \
  ExpectDeniedOnlyInSandbox((fixture), {__VA_ARGS__}, __FILE__, __LINE__)

}  // namespace

SEQ_TEST(Sandbox_IsAvailableOnThisHost) {
  const seq::SandboxSupport support = seq::DetectSandbox();
  CHECK(support.available);
  CHECK(support.landlock_abi >= seq::kMinLandlockAbi);
  CHECK(support.seccomp);
  if (!support.available) {
    seqtest::ReportFailure(__FILE__, __LINE__, support.detail);
  }
}

SEQ_TEST(Sandbox_ProgramCanUseItsOwnAreas) {
  const Fixture fixture;
  CHECK_EQ(Exit(Sandboxed(fixture, {"read", "mine.txt"})), kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"write", "new.txt"})), kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"mkdir", "sub"})), kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"write", "sub/nested.txt"})), kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"truncate", "new.txt"})), kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"unlink", "new.txt"})), kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"readdir", "."})), kAllowed);
  // Inputs are readable.
  CHECK_EQ(
      Exit(Sandboxed(fixture, {"read", (fixture.input / "data.txt").string()})),
      kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"readdir", fixture.input.string()})),
           kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"signal-self"})), kAllowed);
  CHECK(fs::exists(fixture.staging / "sub" / "nested.txt"));
}

SEQ_TEST(Sandbox_DeniesReadingUnrelatedFiles) {
  const Fixture fixture;
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "read", "/etc/passwd");
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "read",
                                (fixture.outside / "secret.txt").string());
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "readdir", "/");
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "readdir", "/etc");
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "readdir", fixture.outside.string());
  // Relative escapes out of staging are denied as well.
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "readdir", "..");
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "read", "/proc/self/environ");
}

SEQ_TEST(Sandbox_DeniesCompilerOwnedArtifacts) {
  const Fixture fixture;
  const std::string source = (fixture.temp / "demo.c").string();
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "read", source);
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "write", source);
  // The control run above appended to the file; restore it.
  Fixture::Write(fixture.temp / "demo.c", "compiler-owned source\n");
  CHECK_EQ(Exit(Sandboxed(fixture, {"truncate", source})), kDenied);
  CHECK_EQ(Exit(Sandboxed(fixture, {"unlink", source})), kDenied);
  CHECK_EQ(Exit(Sandboxed(fixture,
                          {"write", (fixture.temp / "planted.txt").string()})),
           kDenied);
  CHECK_EQ(Fixture::Read(fixture.temp / "demo.c"),
           std::string("compiler-owned source\n"));
  CHECK(!fs::exists(fixture.temp / "planted.txt"));
}

SEQ_TEST(Sandbox_InputsAreReadOnly) {
  const Fixture fixture;
  const std::string data = (fixture.input / "data.txt").string();
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "write", data);
  // The control run above appended to the file; restore it.
  Fixture::Write(fixture.input / "data.txt", "input data\n");
  CHECK_EQ(Exit(Sandboxed(fixture, {"truncate", data})), kDenied);
  CHECK_EQ(Exit(Sandboxed(fixture, {"unlink", data})), kDenied);
  CHECK_EQ(
      Exit(Sandboxed(fixture, {"write", (fixture.input / "new.txt").string()})),
      kDenied);
  CHECK_EQ(Exit(Sandboxed(fixture, {"rename", data, "stolen.txt"})), kDenied);
  CHECK_EQ(Fixture::Read(fixture.input / "data.txt"),
           std::string("input data\n"));
  CHECK(!fs::exists(fixture.input / "new.txt"));
}

SEQ_TEST(Sandbox_DeniesWritesOutsideStaging) {
  const Fixture fixture;
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "write",
                                (fixture.outside / "planted.txt").string());
  fs::remove(fixture.outside / "planted.txt");
  CHECK_EQ(Exit(Sandboxed(fixture, {"write", "../../escaped.txt"})), kDenied);
  CHECK_EQ(Exit(Sandboxed(fixture, {"mkdir", "/tmp/seq-sandbox-escape"})),
           kDenied);
  CHECK(!fs::exists(fixture.outside / "planted.txt"));
  CHECK(!fs::exists("/tmp/seq-sandbox-escape"));
}

SEQ_TEST(Sandbox_DeniesLinksInStaging) {
  // Staging may only ever hold regular files and directories.
  const Fixture fixture;
  CHECK_EQ(Exit(Unrestricted(fixture, {"symlink", "/etc/passwd", "control"})),
           kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"symlink", "/etc/passwd", "link"})),
           kDenied);
  CHECK(!fs::exists(fs::symlink_status(fixture.staging / "link")));
  // A hard link to an input would let the program publish the input itself.
  // The kernel refuses it (as a cross-directory link, EXDEV).
  CHECK(Exit(Sandboxed(fixture, {"link", (fixture.input / "data.txt").string(),
                                 "hard"})) != kAllowed);
  CHECK(!fs::exists(fixture.staging / "hard"));
}

SEQ_TEST(Sandbox_DeniesNetwork) {
  const Fixture fixture;
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "socket");
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "socket-udp");
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "socket-unix");
}

SEQ_TEST(Sandbox_DeniesProcessCreationAndOtherPrograms) {
  const Fixture fixture;
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "fork");
  // /bin/true exists and runs outside the sandbox; inside, nothing but the
  // program's own executable may be executed.
  CHECK_EQ(Exit(Unrestricted(fixture, {"exec", "/bin/true"})), kAllowed);
  CHECK_EQ(Exit(Sandboxed(fixture, {"exec", "/bin/true"})), kDenied);
  CHECK_EQ(Exit(Sandboxed(fixture, {"exec", "/bin/sh"})), kDenied);
}

SEQ_TEST(Sandbox_DeniesSignalsToOtherProcesses) {
  const Fixture fixture;
  EXPECT_DENIED_ONLY_IN_SANDBOX(fixture, "signal-parent");
  // abort() still terminates the program itself.
  const seq::ProcessResult aborted = Sandboxed(fixture, {"abort"});
  CHECK(aborted.launched);
  CHECK(aborted.signal != 0);
}

SEQ_TEST(Sandbox_EnforcesCpuLimit) {
  const Fixture fixture;
  seq::ProcessSpec spec = ProbeSpec(fixture, {"spin"});
  spec.sandbox = seq::RunSandboxPolicy(kProbe, fixture.staging, fixture.input);
  spec.limits.cpu_seconds = 1;
  spec.limits.wall_seconds = 30;
  const seq::ProcessResult result = seq::RunProcess(spec);
  CHECK(result.launched);
  CHECK(!result.timed_out);
  CHECK(result.signal == SIGXCPU || result.signal == SIGKILL);
  CHECK(result.seconds < 10.0);
}

SEQ_TEST(Sandbox_EnforcesWallClockLimit) {
  const Fixture fixture;
  seq::ProcessSpec spec = ProbeSpec(fixture, {"sleep"});
  spec.sandbox = seq::RunSandboxPolicy(kProbe, fixture.staging, fixture.input);
  spec.limits.wall_seconds = 1;
  const seq::ProcessResult result = seq::RunProcess(spec);
  CHECK(result.timed_out);
  CHECK_EQ(result.signal, SIGKILL);
  CHECK(result.seconds < 10.0);
  CHECK(!result.Succeeded());
}

SEQ_TEST(Sandbox_EnforcesOutputLimit) {
  const Fixture fixture;
  seq::ProcessSpec spec = ProbeSpec(fixture, {"flood"});
  spec.sandbox = seq::RunSandboxPolicy(kProbe, fixture.staging, fixture.input);
  spec.limits.output_bytes = 1024 * 1024;
  const seq::ProcessResult result = seq::RunProcess(spec);
  CHECK(result.output_limit_exceeded);
  CHECK(!result.timed_out);
  CHECK(result.out.size() < 64u * 1024u * 1024u);
  CHECK(result.seconds < 10.0);
}

SEQ_TEST(Sandbox_EnforcesFileSizeLimit) {
  const Fixture fixture;
  seq::ProcessSpec spec = ProbeSpec(fixture, {"bigfile", "huge.bin"});
  spec.sandbox = seq::RunSandboxPolicy(kProbe, fixture.staging, fixture.input);
  spec.limits.file_bytes = 2 * 1024 * 1024;
  const seq::ProcessResult result = seq::RunProcess(spec);
  CHECK(result.launched);
  CHECK(!result.Succeeded());
  CHECK(fs::file_size(fixture.staging / "huge.bin") <= 2u * 1024u * 1024u);
}

SEQ_TEST(Sandbox_EnforcesMemoryLimit) {
  const Fixture fixture;
  seq::ProcessSpec spec = ProbeSpec(fixture, {"alloc"});
  spec.sandbox = seq::RunSandboxPolicy(kProbe, fixture.staging, fixture.input);
  spec.limits.memory_bytes = 256u * 1024u * 1024u;
  const seq::ProcessResult result = seq::RunProcess(spec);
  CHECK(result.launched);
  CHECK_EQ(result.exit_code, 1);
}

SEQ_TEST(Sandbox_ProcessEnvironmentIsFixed) {
  const Fixture fixture;
  // Something the child must not inherit.
  const int leaked = open("/dev/null", O_RDONLY);
  setenv("SEQ_TEST_SECRET", "must-not-leak", 1);

  const seq::ProcessResult result =
      seq::RunIsolated(kProbe, fixture.staging, fixture.input,
                       seq::Config::Defaults(), nullptr, nullptr);
  close(leaked);
  unsetenv("SEQ_TEST_SECRET");

  CHECK(result.Succeeded());
  CHECK_EQ(result.out,
           "argc=1\n"
           "env=LC_ALL=C\n"
           "env=TZ=UTC\n"
           "cwd=" +
               fs::canonical(fixture.staging).string() +
               "\n"
               "stdin=eof\n"
               "fd3=fifo\n"
               "fd4=dir\n"
               "open=0 1 2 3 4 \n");
}

SEQ_TEST(Sandbox_CollectsStepRecordsFromDescriptor3) {
  const Fixture fixture;
  seq::ProcessSpec spec = ProbeSpec(fixture, {"report"});
  spec.sandbox = seq::RunSandboxPolicy(kProbe, fixture.staging, fixture.input);
  spec.report_pipe = true;
  const seq::ProcessResult result = seq::RunProcess(spec);
  CHECK(result.Succeeded());
  CHECK_EQ(result.report, std::string("begin 1 probe\nok 1 probe\n"));
  CHECK(result.out.empty());
}

SEQ_TEST(Sandbox_MissingProgramIsALaunchError) {
  const Fixture fixture;
  seq::ProcessSpec spec;
  spec.argv = {(fixture.root / "no-such-program").string()};
  spec.cwd = fixture.staging;
  spec.sandbox =
      seq::RunSandboxPolicy(spec.argv[0], fixture.staging, fixture.input);
  const seq::ProcessResult result = seq::RunProcess(spec);
  CHECK(!result.launched);
  CHECK(!result.Succeeded());
  CHECK(!result.launch_error.empty());

  seq::ProcessSpec relative;
  relative.argv = {"sandbox_probe"};
  CHECK(!seq::RunProcess(relative).launched);
}

SEQ_TEST(BuildSandbox_AllowsCompilerNeedsOnly) {
  const Fixture fixture;
  seq::Config config = seq::Config::Defaults();
  seq::Toolchain toolchain;
  std::string error;
  CHECK(seq::LocateToolchain(config, &toolchain, &error));
  config.Set("paths.home", SEQ_TEST_HOME_DIR);
  const seq::CompilerDriver driver(toolchain, seq::LocateInstallation(config),
                                   config);
  const fs::path work = fixture.temp / "runs" / "r1" / "attempts";
  fs::create_directories(work);

  const auto in_build_sandbox = [&](std::vector<std::string> args) {
    seq::ProcessSpec spec = ProbeSpec(fixture, std::move(args));
    spec.cwd = work;
    seq::SandboxPolicy policy = driver.BuildSandboxPolicy(work);
    policy.read_exec.push_back(kProbe);
    spec.sandbox = policy;
    return Exit(seq::RunProcess(spec));
  };

  // The compiler starts subprocesses, reads the system, and writes its
  // working directory.
  CHECK_EQ(in_build_sandbox({"fork"}), kAllowed);
  CHECK_EQ(in_build_sandbox({"exec", "/bin/true"}), kAllowed);
  CHECK_EQ(in_build_sandbox({"read", "/usr/include/stdio.h"}), kAllowed);
  CHECK_EQ(in_build_sandbox({"write", "object.o"}), kAllowed);
  CHECK_EQ(in_build_sandbox({"write", "/dev/null"}), kAllowed);
  // It has no network and cannot touch anything else: a malicious source
  // file cannot make it read the user's files or write outside its attempt.
  CHECK_EQ(in_build_sandbox({"socket"}), kDenied);
  CHECK_EQ(in_build_sandbox({"socket-unix"}), kDenied);
  CHECK_EQ(in_build_sandbox({"read", "/etc/passwd"}), kDenied);
  CHECK_EQ(in_build_sandbox({"read", "/proc/self/environ"}), kDenied);
  CHECK_EQ(
      in_build_sandbox({"read", (fixture.outside / "secret.txt").string()}),
      kDenied);
  CHECK_EQ(in_build_sandbox({"read", (fixture.input / "data.txt").string()}),
           kDenied);
  CHECK_EQ(
      in_build_sandbox({"write", (fixture.outside / "planted.txt").string()}),
      kDenied);
  CHECK_EQ(in_build_sandbox({"write", "/etc/seq-planted"}), kDenied);
  CHECK_EQ(in_build_sandbox({"write", (fixture.temp / "demo.c").string()}),
           kDenied);
  CHECK_EQ(in_build_sandbox({"signal-parent"}), kDenied);
}

SEQ_TEST(Process_KillsLeftoverProcessGroup) {
  // The probe exits at once but leaves a child that would hold the output
  // pipe open forever. RunProcess must not wait for it.
  const Fixture fixture;
  seq::ProcessSpec spec = ProbeSpec(fixture, {"orphan"});
  spec.limits.wall_seconds = 60;
  const seq::ProcessResult result = seq::RunProcess(spec);
  CHECK(result.launched);
  CHECK_EQ(result.exit_code, 0);
  CHECK(result.seconds < 10.0);
}
