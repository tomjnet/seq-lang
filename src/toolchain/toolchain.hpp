#ifndef SEQ_TOOLCHAIN_TOOLCHAIN_HPP_
#define SEQ_TOOLCHAIN_TOOLCHAIN_HPP_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "project/project.hpp"
#include "support/config.hpp"
#include "support/json.hpp"
#include "support/process.hpp"

namespace seq {

// The native compilers used for generated code.
struct Toolchain {
  std::filesystem::path cc;
  std::filesystem::path cxx;
  // First line of `--version`, recorded in manifests and the cache key.
  std::string cc_version;
  std::string cxx_version;
  // Optional Google Test prefix with include/ and lib/.
  std::filesystem::path gtest_root;

  Json ToJson() const;
};

// Finds gcc and g++ and reads their versions. A missing compiler is a
// prerequisite error, never a generated-code error.
bool LocateToolchain(const Config& config, Toolchain* toolchain,
                     std::string* error);

struct CompileResult {
  // The command ran and succeeded.
  bool ok = false;
  // The command could not run properly: it could not be started, was killed,
  // timed out, or the sandbox failed. Never a candidate for model repair.
  bool infrastructure_failure = false;
  bool isolation_failure = false;
  // Compiler output (stderr, then stdout).
  std::string diagnostics;
  // The command line, for logs.
  std::string command;
  double seconds = 0.0;
};

// Every compiler invocation runs in the build sandbox with `work_dir` as its
// working directory and only place it may write. File arguments are given
// relative to work_dir so diagnostics show short names.
class CompilerDriver {
 public:
  CompilerDriver(const Toolchain& toolchain, const Installation& install,
                 const Config& config);

  // gcc -std=c17 -O3 -Wall -Wextra -Wpedantic -S <source> -o <assembly>
  CompileResult EmitAssembly(const std::filesystem::path& work_dir,
                             const std::string& source,
                             const std::string& assembly) const;
  // gcc <assembly> -o <binary> -static -lseqrt -lm
  CompileResult LinkProgram(const std::filesystem::path& work_dir,
                            const std::string& assembly,
                            const std::string& binary) const;
  // gcc -std=c17 -O3 -DSEQ_NO_MAIN -c <source> -o <object>
  CompileResult CompileTestObject(const std::filesystem::path& work_dir,
                                  const std::string& source,
                                  const std::string& object) const;
  // g++ -std=c++17 <test source> <object> -o <binary> -static -lseqrt
  //     -lgtest_main -lgtest -lpthread -lm
  CompileResult LinkTest(const std::filesystem::path& work_dir,
                         const std::string& test_source,
                         const std::string& object,
                         const std::string& binary) const;

  // Checks that a trivial static C program links (the static C library is
  // installed) and that a trivial Google Test program links.
  CompileResult ProbeStaticLink(const std::filesystem::path& work_dir) const;
  CompileResult ProbeGoogleTest(const std::filesystem::path& work_dir) const;

  // The filesystem view every compiler invocation gets: read and execute the
  // system toolchain, read the seqc runtime, and write only in work_dir.
  SandboxPolicy BuildSandboxPolicy(const std::filesystem::path& work_dir) const;

 private:
  CompileResult Run(const std::filesystem::path& work_dir,
                    std::vector<std::string> argv) const;

  Toolchain toolchain_;
  Installation install_;
  std::uint64_t timeout_seconds_;
  std::uint64_t memory_bytes_;
};

}  // namespace seq

#endif  // SEQ_TOOLCHAIN_TOOLCHAIN_HPP_
