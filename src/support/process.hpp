#ifndef SEQ_SUPPORT_PROCESS_HPP_
#define SEQ_SUPPORT_PROCESS_HPP_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace seq {

// Filesystem view and process rules for a sandboxed child. Everything not
// listed is denied. Network access and signals to other processes are always
// denied inside a sandbox.
struct SandboxPolicy {
  // Files or directories that may be read.
  std::vector<std::filesystem::path> read_only;
  // Files or directories that may be read and executed.
  std::vector<std::filesystem::path> read_exec;
  // Directories in which regular files and subdirectories may be created,
  // read, written, and removed; or single files that may be read and written.
  std::vector<std::filesystem::path> read_write;
  // The compiler needs to start its own subprocesses; generated programs must
  // not create any process.
  bool allow_process_creation = false;
};

// A zero value means "no limit".
struct ProcessLimits {
  std::uint64_t cpu_seconds = 0;
  std::uint64_t wall_seconds = 0;
  std::uint64_t memory_bytes = 0;
  std::uint64_t file_bytes = 0;
  // Combined stdout and stderr. The child is killed when it writes more.
  std::uint64_t output_bytes = 0;
};

struct ProcessSpec {
  // argv[0] must be an absolute path; PATH is never searched.
  std::vector<std::string> argv;
  // Complete environment as NAME=value entries. Nothing is inherited.
  std::vector<std::string> env;
  std::filesystem::path cwd;
  // When set, the child runs sandboxed. If the sandbox cannot be applied the
  // child is not executed.
  std::optional<SandboxPolicy> sandbox;
  ProcessLimits limits;
  // Give the child a pipe on descriptor 3 and collect what it writes there.
  bool report_pipe = false;
  // When nonempty, the child receives this directory, opened read-only, as
  // descriptor 4.
  std::filesystem::path input_dir;
  // When set, output is copied here as it arrives, in addition to being
  // captured.
  std::ostream* live_stdout = nullptr;
  std::ostream* live_stderr = nullptr;
};

struct ProcessResult {
  // False if the child could not be executed at all; see launch_error.
  bool launched = false;
  std::string launch_error;
  // The sandbox could not be applied. Never fall back to running unsandboxed.
  bool isolation_failure = false;
  int exit_code = -1;
  // Nonzero when the child was terminated by a signal.
  int signal = 0;
  bool timed_out = false;
  bool output_limit_exceeded = false;
  bool cancelled = false;
  std::string out;
  std::string err;
  std::string report;
  double seconds = 0.0;

  bool Succeeded() const {
    return launched && signal == 0 && exit_code == 0 && !timed_out &&
           !output_limit_exceeded && !cancelled;
  }
};

// Runs a child to completion. The child is placed in its own process group
// and the whole group is killed on timeout, cancellation, or when the child
// exits.
ProcessResult RunProcess(const ProcessSpec& spec);

struct SandboxSupport {
  bool available = false;
  // Landlock ABI version, or 0 when Landlock is unavailable.
  int landlock_abi = 0;
  bool seccomp = false;
  std::string kernel;
  // One-line explanation, suitable for `seqc doctor` and error messages.
  std::string detail;
};

// Minimum Landlock ABI: version 3 adds truncation control, without which a
// sandboxed program could truncate its read-only inputs.
inline constexpr int kMinLandlockAbi = 3;

SandboxSupport DetectSandbox();

// Searches PATH for a program. A name containing a slash is checked as given.
std::optional<std::filesystem::path> FindProgram(std::string_view name);

// Requests cancellation of the running child; safe to call from a signal
// handler. Installed for SIGINT and SIGTERM by InstallCancellationHandlers.
void RequestCancellation();
bool CancellationRequested();
void InstallCancellationHandlers();

// A long-running helper process, such as the inference server. Not sandboxed.
class BackgroundProcess {
 public:
  BackgroundProcess() = default;
  ~BackgroundProcess();
  BackgroundProcess(const BackgroundProcess&) = delete;
  BackgroundProcess& operator=(const BackgroundProcess&) = delete;

  // Starts argv with stdout and stderr appended to log_path.
  bool Start(const std::vector<std::string>& argv,
             const std::filesystem::path& log_path, std::string* error);
  bool Running();
  void Stop();

 private:
  long pid_ = -1;
};

// Exclusive advisory lock held for the lifetime of the object.
class FileLock {
 public:
  FileLock() = default;
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  // Returns false immediately if another process holds the lock (*busy is set
  // to true) or the lock file cannot be opened.
  bool TryAcquire(const std::filesystem::path& path, bool* busy,
                  std::string* error);

 private:
  int fd_ = -1;
};

struct HttpResponse {
  bool ok = false;  // Transport succeeded; check status separately.
  std::string error;
  int status = 0;
  std::string body;
};

// Plain HTTP/1.1 request to a loopback or LAN host. No TLS: v0.1 only talks
// to an inference server on the same machine.
HttpResponse HttpRequest(const std::string& host, int port,
                         const std::string& method, const std::string& path,
                         const std::string& body, int timeout_seconds);

// Asks the kernel for a currently unused loopback TCP port.
int FindFreeLoopbackPort();

}  // namespace seq

#endif  // SEQ_SUPPORT_PROCESS_HPP_
