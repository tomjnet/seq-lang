// Linux implementation of process execution and the sandbox.
//
// The sandbox is applied by the child between fork and exec and needs no
// privileges and no user namespaces:
//   * Landlock restricts the filesystem view (and TCP, and signal scope, on
//     kernels that support those),
//   * a seccomp filter denies socket creation, signals to other processes,
//     and, for generated programs, process creation,
//   * no_new_privs and resource limits bound the rest.
// See docs/security.md.

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>

#include "support/process.hpp"
#include "support/util.hpp"

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#define SYS_io_uring_register 427
#endif

namespace seq {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

// --- Landlock ----------------------------------------------------------------
// The constants are spelled out here so the build does not depend on how new
// the installed kernel headers are.

constexpr std::uint64_t kFsExecute = 1ULL << 0;
constexpr std::uint64_t kFsWriteFile = 1ULL << 1;
constexpr std::uint64_t kFsReadFile = 1ULL << 2;
constexpr std::uint64_t kFsReadDir = 1ULL << 3;
constexpr std::uint64_t kFsRemoveDir = 1ULL << 4;
constexpr std::uint64_t kFsRemoveFile = 1ULL << 5;
constexpr std::uint64_t kFsMakeDir = 1ULL << 7;
constexpr std::uint64_t kFsMakeReg = 1ULL << 8;
constexpr std::uint64_t kFsTruncate = 1ULL << 14;  // ABI 3

constexpr std::uint64_t kFsAllAbi1 = (1ULL << 13) - 1;
constexpr std::uint64_t kFsRefer = 1ULL << 13;     // ABI 2
constexpr std::uint64_t kFsIoctlDev = 1ULL << 15;  // ABI 5

constexpr std::uint64_t kNetBindTcp = 1ULL << 0;         // ABI 4
constexpr std::uint64_t kNetConnectTcp = 1ULL << 1;      // ABI 4
constexpr std::uint64_t kScopeAbstractUnix = 1ULL << 0;  // ABI 6
constexpr std::uint64_t kScopeSignal = 1ULL << 1;        // ABI 6

constexpr std::uint64_t kFsFileRights =
    kFsExecute | kFsWriteFile | kFsReadFile | kFsTruncate | kFsIoctlDev;

constexpr unsigned kCreateRulesetVersion = 1U << 0;
constexpr int kRulePathBeneath = 1;

struct RulesetAttr {
  std::uint64_t handled_access_fs;
  std::uint64_t handled_access_net;
  std::uint64_t scoped;
};

struct __attribute__((packed)) PathBeneathAttr {
  std::uint64_t allowed_access;
  std::int32_t parent_fd;
};

int LandlockAbi() {
  const long abi =
      syscall(SYS_landlock_create_ruleset, nullptr, 0, kCreateRulesetVersion);
  return abi < 0 ? 0 : static_cast<int>(abi);
}

std::uint64_t HandledFsRights(int abi) {
  std::uint64_t rights = kFsAllAbi1;
  if (abi >= 2) rights |= kFsRefer;
  if (abi >= 3) rights |= kFsTruncate;
  if (abi >= 5) rights |= kFsIoctlDev;
  return rights;
}

// Writes a message to the status pipe and exits. Used only in the child.
[[noreturn]] void ChildFail(int status_fd, const char* what, const char* detail,
                            int error_number, int exit_code) {
  std::string message = what;
  if (detail != nullptr && *detail != '\0') {
    message += " ";
    message += detail;
  }
  if (error_number != 0) {
    message += ": ";
    message += std::strerror(error_number);
  }
  ssize_t ignored = write(status_fd, message.data(), message.size());
  (void)ignored;
  _exit(exit_code);
}

// One granted path, held open by the supervisor.
struct LandlockRule {
  int fd = -1;
  std::uint64_t access = 0;
  std::string path;
};

// Opens every path the policy grants and works out the rights for each.
//
// The supervisor keeps these descriptors open until the child has exited. A
// Landlock rule is attached to the kernel's in-memory object for a file, and
// some filesystems (9p, which WSL uses for Windows drives) discard that
// object as soon as its last user closes it. A later lookup of the same path
// then yields a new object that no rule matches, and access is denied. Holding
// the descriptor keeps the object, and so the rule, alive. If this ever fails
// the result is a denial, never extra access.
bool OpenLandlockRules(const SandboxPolicy& policy,
                       std::vector<LandlockRule>* rules, std::string* error) {
  const std::uint64_t read = kFsReadFile | kFsReadDir;
  const std::uint64_t write = read | kFsWriteFile | kFsRemoveDir |
                              kFsRemoveFile | kFsMakeDir | kFsMakeReg |
                              kFsTruncate;
  const auto add = [&](const std::vector<fs::path>& paths,
                       std::uint64_t access) {
    for (const fs::path& path : paths) {
      const int opened = open(path.c_str(), O_PATH | O_CLOEXEC);
      if (opened < 0) {
        // A path that does not exist grants nothing, which is the safe
        // direction.
        if (errno == ENOENT || errno == ENOTDIR) continue;
        *error = "sandbox: cannot open " + path.string() + ": " +
                 std::strerror(errno);
        return false;
      }
      // Keep rule descriptors clear of the numbers the child rearranges.
      const int fd = fcntl(opened, F_DUPFD_CLOEXEC, 20);
      const int dup_errno = errno;
      close(opened);
      struct stat info;
      if (fd < 0 || fstat(fd, &info) != 0) {
        *error = "sandbox: cannot hold " + path.string() + ": " +
                 std::strerror(fd < 0 ? dup_errno : errno);
        if (fd >= 0) close(fd);
        return false;
      }
      LandlockRule rule;
      rule.fd = fd;
      rule.access = S_ISDIR(info.st_mode) ? access : access & kFsFileRights;
      rule.path = path.string();
      rules->push_back(std::move(rule));
    }
    return true;
  };
  return add(policy.read_only, read) &&
         add(policy.read_exec, read | kFsExecute) &&
         add(policy.read_write, write);
}

void CloseLandlockRules(std::vector<LandlockRule>* rules) {
  for (LandlockRule& rule : *rules) {
    if (rule.fd >= 0) close(rule.fd);
    rule.fd = -1;
  }
}

// Runs in the child, between fork and exec.
void ApplyLandlock(const std::vector<LandlockRule>& rules, int abi,
                   int status_fd) {
  RulesetAttr attr{};
  attr.handled_access_fs = HandledFsRights(abi);
  std::size_t attr_size = sizeof(std::uint64_t);
  if (abi >= 4) {
    attr.handled_access_net = kNetBindTcp | kNetConnectTcp;
    attr_size = 2 * sizeof(std::uint64_t);
  }
  if (abi >= 6) {
    attr.scoped = kScopeAbstractUnix | kScopeSignal;
    attr_size = 3 * sizeof(std::uint64_t);
  }
  const int ruleset_fd = static_cast<int>(
      syscall(SYS_landlock_create_ruleset, &attr, attr_size, 0));
  if (ruleset_fd < 0) {
    ChildFail(status_fd, "sandbox: landlock_create_ruleset", "", errno, 126);
  }

  for (const LandlockRule& rule : rules) {
    PathBeneathAttr path_attr{};
    path_attr.allowed_access = rule.access & attr.handled_access_fs;
    path_attr.parent_fd = rule.fd;
    if (path_attr.allowed_access == 0) continue;
    if (syscall(SYS_landlock_add_rule, ruleset_fd, kRulePathBeneath, &path_attr,
                0) != 0) {
      ChildFail(status_fd, "sandbox: landlock_add_rule", rule.path.c_str(),
                errno, 126);
    }
  }

  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    ChildFail(status_fd, "sandbox: no_new_privs", "", errno, 126);
  }
  if (syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0) {
    ChildFail(status_fd, "sandbox: landlock_restrict_self", "", errno, 126);
  }
  close(ruleset_fd);
}

// --- seccomp -----------------------------------------------------------------

struct SeccompFilter {
  std::vector<sock_filter> code;
  // Index of the instruction whose constant must be set to the child's pid.
  std::size_t pid_index = 0;
};

SeccompFilter BuildSeccompFilter(bool allow_process_creation) {
  const std::uint32_t deny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
  const auto stmt = [](std::uint16_t code, std::uint32_t k) {
    return sock_filter{code, 0, 0, k};
  };
  const auto jump = [](std::uint16_t code, std::uint32_t k, std::uint8_t jt,
                       std::uint8_t jf) {
    return sock_filter{code, jt, jf, k};
  };

  SeccompFilter filter;
  std::vector<sock_filter>& code = filter.code;

  // Only the native x86_64 ABI is accepted.
  code.push_back(
      stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
  code.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
  code.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
  code.push_back(
      stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));
  // x32 system calls carry bit 30.
  code.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000U, 0, 1));
  code.push_back(stmt(BPF_RET | BPF_K, deny));

  // kill and tgkill are allowed only when aimed at the caller itself, so that
  // abort() and raise() keep working.
  code.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, SYS_kill, 1, 0));
  code.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, SYS_tgkill, 0, 4));
  code.push_back(
      stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
  filter.pid_index = code.size();
  code.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
  code.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  code.push_back(stmt(BPF_RET | BPF_K, deny));
  // Fallthrough for every other system call: the accumulator still holds nr.

  std::vector<long> denied = {
      // Network.
      SYS_socket,
      SYS_socketpair,
      SYS_connect,
      SYS_bind,
      SYS_listen,
      SYS_accept,
      SYS_accept4,
      // Signals to other processes.
      SYS_tkill,
      SYS_rt_sigqueueinfo,
      SYS_rt_tgsigqueueinfo,
      SYS_pidfd_send_signal,
      // Inspection and control of other processes or the system.
      SYS_ptrace,
      SYS_process_vm_readv,
      SYS_process_vm_writev,
      SYS_mount,
      SYS_umount2,
      SYS_pivot_root,
      SYS_chroot,
      SYS_unshare,
      SYS_setns,
      SYS_bpf,
      SYS_perf_event_open,
      SYS_userfaultfd,
      SYS_keyctl,
      SYS_add_key,
      SYS_request_key,
      SYS_init_module,
      SYS_finit_module,
      SYS_delete_module,
      SYS_kexec_load,
      SYS_reboot,
      SYS_swapon,
      SYS_swapoff,
      SYS_acct,
      SYS_settimeofday,
      SYS_clock_settime,
      SYS_io_uring_setup,
      SYS_io_uring_enter,
      SYS_io_uring_register,
  };
  if (!allow_process_creation) {
    const long process_calls[] = {SYS_fork,   SYS_vfork,    SYS_clone,
                                  SYS_clone3, SYS_execveat, SYS_memfd_create};
    denied.insert(denied.end(), std::begin(process_calls),
                  std::end(process_calls));
  }
  for (const long nr : denied) {
    code.push_back(
        jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(nr), 0, 1));
    code.push_back(stmt(BPF_RET | BPF_K, deny));
  }
  code.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  return filter;
}

void ApplySeccomp(SeccompFilter* filter, int status_fd) {
  filter->code[filter->pid_index].k = static_cast<std::uint32_t>(getpid());
  sock_fprog program{};
  program.len = static_cast<unsigned short>(filter->code.size());
  program.filter = filter->code.data();
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    ChildFail(status_fd, "sandbox: no_new_privs", "", errno, 126);
  }
  if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
    ChildFail(status_fd, "sandbox: seccomp filter", "", errno, 126);
  }
}

// --- Child setup -------------------------------------------------------------

void SetLimit(int resource, std::uint64_t soft, std::uint64_t hard,
              int status_fd) {
  struct rlimit limit;
  limit.rlim_cur = static_cast<rlim_t>(soft);
  limit.rlim_max = static_cast<rlim_t>(hard);
  if (setrlimit(resource, &limit) != 0) {
    ChildFail(status_fd, "cannot set resource limit", "", errno, 126);
  }
}

int LiftFd(int fd, int status_fd) {
  if (fd < 0) return -1;
  const int lifted = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  if (lifted < 0 && status_fd >= 0) {
    ChildFail(status_fd, "cannot duplicate descriptor", "", errno, 126);
  }
  return lifted;
}

struct ChildFds {
  int out = -1;
  int err = -1;
  int report = -1;
  int status = -1;
  int null = -1;
  int input = -1;
};

[[noreturn]] void ChildMain(const ProcessSpec& spec, ChildFds fds,
                            const std::vector<LandlockRule>& rules,
                            int landlock_abi, SeccompFilter* seccomp,
                            char* const* argv, char* const* envp) {
  setpgid(0, 0);

  // Move every descriptor we still need out of the 0-9 range before wiring up
  // the standard ones, so no dup2 can clobber a source.
  const int status_fd = LiftFd(fds.status, -1);
  if (status_fd < 0) _exit(126);
  const int out_fd = LiftFd(fds.out, status_fd);
  const int err_fd = LiftFd(fds.err, status_fd);
  const int null_fd = LiftFd(fds.null, status_fd);
  const int report_fd = LiftFd(fds.report, status_fd);
  const int input_fd = LiftFd(fds.input, status_fd);

  if (dup2(null_fd, 0) < 0 || dup2(out_fd, 1) < 0 || dup2(err_fd, 2) < 0) {
    ChildFail(status_fd, "cannot set up standard streams", "", errno, 126);
  }
  if (report_fd >= 0) {
    if (dup2(report_fd, 3) < 0) {
      ChildFail(status_fd, "cannot set up report descriptor", "", errno, 126);
    }
  } else {
    close(3);
  }
  if (input_fd >= 0) {
    if (dup2(input_fd, 4) < 0) {
      ChildFail(status_fd, "cannot set up input descriptor", "", errno, 126);
    }
  } else {
    close(4);
  }
  // Nothing else from the parent may leak into the child.
  if (syscall(SYS_close_range, 5U, ~0U, 4U /* CLOSE_RANGE_CLOEXEC */) != 0) {
    for (int fd = 5; fd < 4096; ++fd) {
      const int flags = fcntl(fd, F_GETFD);
      if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }

  if (!spec.cwd.empty() && chdir(spec.cwd.c_str()) != 0) {
    ChildFail(status_fd, "cannot enter", spec.cwd.c_str(), errno, 126);
  }

  SetLimit(RLIMIT_CORE, 0, 0, status_fd);
  if (spec.limits.cpu_seconds != 0) {
    SetLimit(RLIMIT_CPU, spec.limits.cpu_seconds, spec.limits.cpu_seconds + 1,
             status_fd);
  }
  if (spec.limits.memory_bytes != 0) {
    SetLimit(RLIMIT_AS, spec.limits.memory_bytes, spec.limits.memory_bytes,
             status_fd);
  }
  if (spec.limits.file_bytes != 0) {
    SetLimit(RLIMIT_FSIZE, spec.limits.file_bytes, spec.limits.file_bytes,
             status_fd);
  }

  if (spec.sandbox.has_value()) {
    SetLimit(RLIMIT_NOFILE, 256, 256, status_fd);
    ApplyLandlock(rules, landlock_abi, status_fd);
    ApplySeccomp(seccomp, status_fd);
  }

  execve(argv[0], argv, envp);
  ChildFail(status_fd, "cannot execute", argv[0], errno, 127);
}

void SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void CloseFd(int* fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

volatile std::sig_atomic_t g_cancel_requested = 0;

void HandleCancelSignal(int) { g_cancel_requested = 1; }

}  // namespace

void RequestCancellation() { g_cancel_requested = 1; }

bool CancellationRequested() { return g_cancel_requested != 0; }

void InstallCancellationHandlers() {
  struct sigaction action{};
  action.sa_handler = HandleCancelSignal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  // A child that closes its pipes early must not kill the compiler.
  signal(SIGPIPE, SIG_IGN);
}

SandboxSupport DetectSandbox() {
  SandboxSupport support;
  struct utsname names;
  if (uname(&names) == 0) {
    support.kernel = std::string(names.sysname) + " " + names.release;
  }
#if !defined(__x86_64__)
  support.detail = "the sandbox is only implemented for x86_64";
  return support;
#else
  support.landlock_abi = LandlockAbi();
  support.seccomp = prctl(PR_GET_SECCOMP, 0, 0, 0, 0) >= 0;
  if (support.landlock_abi == 0) {
    support.detail =
        "Landlock is not available (kernel 6.2 or newer with Landlock enabled "
        "is required)";
    return support;
  }
  if (support.landlock_abi < kMinLandlockAbi) {
    support.detail = "Landlock ABI " + std::to_string(support.landlock_abi) +
                     " is too old; ABI " + std::to_string(kMinLandlockAbi) +
                     " (kernel 6.2) or newer is required";
    return support;
  }
  if (!support.seccomp) {
    support.detail = "seccomp filtering is not available on this kernel";
    return support;
  }
  support.available = true;
  support.detail = "Landlock ABI " + std::to_string(support.landlock_abi) +
                   ", seccomp filter";
  return support;
#endif
}

ProcessResult RunProcess(const ProcessSpec& spec) {
  ProcessResult result;
  if (spec.argv.empty() || spec.argv[0].empty() || spec.argv[0][0] != '/') {
    result.launch_error = "program path must be absolute";
    return result;
  }

  int landlock_abi = 0;
  SeccompFilter seccomp;
  std::vector<LandlockRule> rules;
  if (spec.sandbox.has_value()) {
    const SandboxSupport support = DetectSandbox();
    if (!support.available) {
      result.isolation_failure = true;
      result.launch_error = support.detail;
      return result;
    }
    landlock_abi = support.landlock_abi;
    seccomp = BuildSeccompFilter(spec.sandbox->allow_process_creation);
    if (!OpenLandlockRules(*spec.sandbox, &rules, &result.launch_error)) {
      result.isolation_failure = true;
      CloseLandlockRules(&rules);
      return result;
    }
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int report_pipe[2] = {-1, -1};
  int status_pipe[2] = {-1, -1};
  int null_fd = -1;
  int input_fd = -1;
  const auto close_all = [&] {
    for (int* fd : {&out_pipe[0], &out_pipe[1], &err_pipe[0], &err_pipe[1],
                    &report_pipe[0], &report_pipe[1], &status_pipe[0],
                    &status_pipe[1], &null_fd, &input_fd}) {
      CloseFd(fd);
    }
    CloseLandlockRules(&rules);
  };
  const auto fail = [&](const std::string& message) {
    result.launch_error = message + ": " + std::strerror(errno);
    close_all();
    return result;
  };

  if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0 ||
      pipe2(status_pipe, O_CLOEXEC) != 0) {
    return fail("cannot create pipe");
  }
  if (spec.report_pipe && pipe2(report_pipe, O_CLOEXEC) != 0) {
    return fail("cannot create pipe");
  }
  null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (null_fd < 0) return fail("cannot open /dev/null");
  if (!spec.input_dir.empty()) {
    input_fd = open(spec.input_dir.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (input_fd < 0) return fail("cannot open " + spec.input_dir.string());
  }

  std::vector<char*> argv;
  for (const std::string& arg : spec.argv) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  std::vector<char*> envp;
  for (const std::string& entry : spec.env) {
    envp.push_back(const_cast<char*>(entry.c_str()));
  }
  envp.push_back(nullptr);

  const Clock::time_point start = Clock::now();
  const pid_t pid = fork();
  if (pid < 0) return fail("cannot fork");
  if (pid == 0) {
    ChildFds fds;
    fds.out = out_pipe[1];
    fds.err = err_pipe[1];
    fds.report = report_pipe[1];
    fds.status = status_pipe[1];
    fds.null = null_fd;
    fds.input = input_fd;
    ChildMain(spec, fds, rules, landlock_abi, &seccomp, argv.data(),
              envp.data());
  }

  setpgid(pid, pid);
  CloseFd(&out_pipe[1]);
  CloseFd(&err_pipe[1]);
  CloseFd(&report_pipe[1]);
  CloseFd(&status_pipe[1]);
  CloseFd(&null_fd);
  CloseFd(&input_fd);

  struct Stream {
    int* fd;
    std::string* sink;
    std::ostream* live;
    bool counts_toward_limit;
  };
  std::string status_text;
  Stream streams[] = {
      {&out_pipe[0], &result.out, spec.live_stdout, true},
      {&err_pipe[0], &result.err, spec.live_stderr, true},
      {&report_pipe[0], &result.report, nullptr, false},
      {&status_pipe[0], &status_text, nullptr, false},
  };
  for (const Stream& stream : streams) {
    if (*stream.fd >= 0) SetNonBlocking(*stream.fd);
  }

  const auto kill_group = [&] {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
  };
  const auto over_output_limit = [&] {
    return spec.limits.output_bytes != 0 &&
           result.out.size() + result.err.size() > spec.limits.output_bytes;
  };
  // Reads whatever is available. Returns the number of streams still open.
  // A child that writes as fast as it is read must not keep this loop busy
  // forever, so reading stops as soon as the output limit is passed; output
  // beyond the limit is neither kept nor relayed.
  const auto drain = [&]() {
    int open_streams = 0;
    char buffer[8192];
    for (Stream& stream : streams) {
      if (*stream.fd < 0) continue;
      while (true) {
        if (stream.counts_toward_limit && over_output_limit()) break;
        const ssize_t got = read(*stream.fd, buffer, sizeof(buffer));
        if (got > 0) {
          stream.sink->append(buffer, static_cast<std::size_t>(got));
          if (stream.live != nullptr) {
            stream.live->write(buffer, got);
            stream.live->flush();
          }
          continue;
        }
        if (got == 0) {
          CloseFd(stream.fd);
        } else if (errno == EINTR) {
          continue;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          CloseFd(stream.fd);
        }
        break;
      }
      if (*stream.fd >= 0) ++open_streams;
    }
    return open_streams;
  };

  int wait_status = 0;
  bool exited = false;
  bool killed = false;
  while (!exited) {
    struct pollfd poll_fds[4];
    nfds_t count = 0;
    for (const Stream& stream : streams) {
      if (*stream.fd >= 0) {
        poll_fds[count].fd = *stream.fd;
        poll_fds[count].events = POLLIN;
        poll_fds[count].revents = 0;
        ++count;
      }
    }
    if (count > 0) {
      poll(poll_fds, count, 50);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    drain();

    if (!killed) {
      const double elapsed =
          std::chrono::duration<double>(Clock::now() - start).count();
      if (spec.limits.wall_seconds != 0 &&
          elapsed > static_cast<double>(spec.limits.wall_seconds)) {
        result.timed_out = true;
        killed = true;
      } else if (over_output_limit()) {
        result.output_limit_exceeded = true;
        killed = true;
        // Nothing more is read from the child's output.
        CloseFd(&out_pipe[0]);
        CloseFd(&err_pipe[0]);
      } else if (CancellationRequested()) {
        result.cancelled = true;
        killed = true;
      }
      if (killed) kill_group();
    }

    const pid_t waited = waitpid(pid, &wait_status, WNOHANG);
    if (waited == pid) {
      exited = true;
    } else if (waited < 0 && errno != EINTR) {
      exited = true;
      wait_status = 0;
    }
  }

  // Anything the child left behind in its process group dies with it; after
  // that the pipes reach end of file.
  kill(-pid, SIGKILL);
  if (over_output_limit()) {
    result.output_limit_exceeded = true;
    CloseFd(&out_pipe[0]);
    CloseFd(&err_pipe[0]);
  }
  const Clock::time_point drain_deadline =
      Clock::now() + std::chrono::seconds(2);
  while (drain() > 0 && Clock::now() < drain_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  close_all();

  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();

  if (!status_text.empty()) {
    result.launched = false;
    result.launch_error = status_text;
    result.isolation_failure = StartsWith(status_text, "sandbox:");
    return result;
  }
  result.launched = true;
  if (WIFEXITED(wait_status)) {
    result.exit_code = WEXITSTATUS(wait_status);
  } else if (WIFSIGNALED(wait_status)) {
    result.signal = WTERMSIG(wait_status);
  }
  return result;
}

std::optional<fs::path> FindProgram(std::string_view name) {
  if (name.empty()) return std::nullopt;
  std::error_code ec;
  const auto usable = [&](const fs::path& candidate) {
    return fs::is_regular_file(candidate, ec) &&
           access(candidate.c_str(), X_OK) == 0;
  };
  if (name.find('/') != std::string_view::npos) {
    const fs::path candidate = fs::absolute(fs::path(name), ec);
    if (!ec && usable(candidate)) return candidate;
    return std::nullopt;
  }
  const char* path_env = std::getenv("PATH");
  const std::string search =
      path_env != nullptr ? path_env : "/usr/local/bin:/usr/bin:/bin";
  std::size_t start = 0;
  while (start <= search.size()) {
    std::size_t end = search.find(':', start);
    if (end == std::string::npos) end = search.size();
    if (end > start) {
      const fs::path candidate =
          fs::path(search.substr(start, end - start)) / std::string(name);
      if (candidate.is_absolute() && usable(candidate)) return candidate;
    }
    start = end + 1;
  }
  return std::nullopt;
}

// --- BackgroundProcess -------------------------------------------------------

BackgroundProcess::~BackgroundProcess() { Stop(); }

bool BackgroundProcess::Start(const std::vector<std::string>& argv,
                              const fs::path& log_path, std::string* error) {
  if (argv.empty()) {
    *error = "no program given";
    return false;
  }
  const int log_fd =
      open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (log_fd < 0) {
    *error = "cannot open " + log_path.string() + ": " + std::strerror(errno);
    return false;
  }
  const int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  std::vector<char*> args;
  for (const std::string& arg : argv) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    *error = std::string("cannot fork: ") + std::strerror(errno);
    close(log_fd);
    if (null_fd >= 0) close(null_fd);
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);
    if (null_fd >= 0) dup2(null_fd, 0);
    dup2(log_fd, 1);
    dup2(log_fd, 2);
    execv(args[0], args.data());
    _exit(127);
  }
  close(log_fd);
  if (null_fd >= 0) close(null_fd);
  pid_ = pid;
  return true;
}

bool BackgroundProcess::Running() {
  if (pid_ <= 0) return false;
  int status = 0;
  const pid_t waited = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (waited == 0) return true;
  pid_ = -1;
  return false;
}

void BackgroundProcess::Stop() {
  if (pid_ <= 0) return;
  const pid_t pid = static_cast<pid_t>(pid_);
  kill(pid, SIGTERM);
  for (int i = 0; i < 60; ++i) {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) != 0) {
      pid_ = -1;
      kill(-pid, SIGKILL);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  pid_ = -1;
}

// --- FileLock ----------------------------------------------------------------

FileLock::~FileLock() {
  if (fd_ >= 0) close(fd_);
}

bool FileLock::TryAcquire(const fs::path& path, bool* busy,
                          std::string* error) {
  *busy = false;
  fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    *error = "cannot open " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    *busy = errno == EWOULDBLOCK;
    *error = "cannot lock " + path.string() + ": " + std::strerror(errno);
    close(fd_);
    fd_ = -1;
    return false;
  }
  return true;
}

// --- HTTP --------------------------------------------------------------------

namespace {

bool WaitFor(int fd, short events, Clock::time_point deadline) {
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              Clock::now());
    if (remaining.count() <= 0 || CancellationRequested()) return false;
    struct pollfd item{fd, events, 0};
    const int ready =
        poll(&item, 1,
             static_cast<int>(std::min<long long>(remaining.count(), 200)));
    if (ready > 0) return true;
    if (ready < 0 && errno != EINTR) return false;
  }
}

bool DecodeChunked(const std::string& raw, std::string* body) {
  std::size_t pos = 0;
  while (true) {
    const std::size_t line_end = raw.find("\r\n", pos);
    if (line_end == std::string::npos) return false;
    const std::string size_text = raw.substr(pos, line_end - pos);
    char* end = nullptr;
    const unsigned long size = std::strtoul(size_text.c_str(), &end, 16);
    if (end == size_text.c_str()) return false;
    pos = line_end + 2;
    if (size == 0) return true;
    if (pos + size > raw.size()) return false;
    body->append(raw, pos, size);
    pos += size + 2;
  }
}

}  // namespace

HttpResponse HttpRequest(const std::string& host, int port,
                         const std::string& method, const std::string& path,
                         const std::string& body, int timeout_seconds) {
  HttpResponse response;
  const Clock::time_point deadline =
      Clock::now() + std::chrono::seconds(timeout_seconds);

  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* addresses = nullptr;
  const std::string service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0 ||
      addresses == nullptr) {
    response.error = "cannot resolve " + host;
    return response;
  }

  int fd = -1;
  std::string connect_error = "cannot connect";
  for (struct addrinfo* a = addresses; a != nullptr; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                a->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
    if (errno == EINPROGRESS && WaitFor(fd, POLLOUT, deadline)) {
      int so_error = 0;
      socklen_t length = sizeof(so_error);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length);
      if (so_error == 0) break;
      connect_error = std::strerror(so_error);
    } else {
      connect_error = std::strerror(errno);
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);
  if (fd < 0) {
    response.error =
        "cannot connect to " + host + ":" + service + ": " + connect_error;
    return response;
  }

  std::string request = method + " " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + ":" + service + "\r\n";
  request += "Connection: close\r\n";
  request += "Accept: application/json\r\n";
  if (!body.empty() || method == "POST") {
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "\r\n";
  request += body;

  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t wrote =
        send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (wrote > 0) {
      sent += static_cast<std::size_t>(wrote);
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      if (!WaitFor(fd, POLLOUT, deadline)) {
        response.error = "timed out sending request";
        close(fd);
        return response;
      }
    } else {
      response.error = std::string("send failed: ") + std::strerror(errno);
      close(fd);
      return response;
    }
  }

  constexpr std::size_t kMaxResponse = 64u * 1024u * 1024u;
  std::string raw;
  char buffer[16384];
  while (true) {
    const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
    if (got > 0) {
      raw.append(buffer, static_cast<std::size_t>(got));
      if (raw.size() > kMaxResponse) {
        response.error = "response too large";
        close(fd);
        return response;
      }
    } else if (got == 0) {
      break;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      if (!WaitFor(fd, POLLIN, deadline)) {
        response.error = CancellationRequested()
                             ? "cancelled"
                             : "timed out waiting for response";
        close(fd);
        return response;
      }
    } else {
      response.error = std::string("receive failed: ") + std::strerror(errno);
      close(fd);
      return response;
    }
  }
  close(fd);

  const std::size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos || !StartsWith(raw, "HTTP/")) {
    response.error = "malformed HTTP response";
    return response;
  }
  const std::size_t first_space = raw.find(' ');
  response.status = std::atoi(raw.c_str() + first_space + 1);
  const std::string headers = ToLower(raw.substr(0, header_end));
  const std::string payload = raw.substr(header_end + 4);
  if (headers.find("transfer-encoding: chunked") != std::string::npos) {
    if (!DecodeChunked(payload, &response.body)) {
      response.error = "malformed chunked response";
      return response;
    }
  } else {
    response.body = payload;
    const std::size_t length_at = headers.find("content-length:");
    if (length_at != std::string::npos) {
      const unsigned long length =
          std::strtoul(headers.c_str() + length_at + 15, nullptr, 10);
      if (response.body.size() < length) {
        response.error = "truncated HTTP response";
        return response;
      }
      response.body.resize(length);
    }
  }
  response.ok = true;
  return response;
}

int FindFreeLoopbackPort() {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return 0;
  struct sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  int port = 0;
  socklen_t length = sizeof(address);
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) ==
          0 &&
      getsockname(fd, reinterpret_cast<struct sockaddr*>(&address), &length) ==
          0) {
    port = ntohs(address.sin_port);
  }
  close(fd);
  return port;
}

}  // namespace seq
