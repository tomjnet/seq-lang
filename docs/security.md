# Security and isolation

`seqc` treats three things as untrusted: the model's responses, the C and C++ code built from them, and the contents of `input/`. Prompting and source inspection do not enforce anything. The kernel does.

## What is enforced

Every compiler invocation and every run of generated code is a child process that sandboxes itself between `fork` and `exec`. No privileges and no user namespaces are needed.

| Mechanism | Effect |
| --- | --- |
| Landlock | The child can reach only the paths it was granted. Everything else on the filesystem is denied, whatever the file permissions say. |
| seccomp filter | Denies creating sockets, signalling other processes, tracing, mounting, namespaces, `bpf`, `io_uring`, and similar. For generated programs it also denies creating processes. |
| `no_new_privs` | A setuid program cannot raise privileges. |
| Resource limits | CPU time, address space, file size, open files, no core dumps. |
| Supervisor | Wall-clock limit, limit on stdout plus stderr, and a kill of the whole process group on timeout, cancellation, or exit. |

If the sandbox cannot be applied, the child is not executed and the build stops with exit status 7. There is no fallback to unrestricted execution.

### Run sandbox: generated programs and generated tests

| Access | Granted |
| --- | --- |
| Read and execute | The program's own executable, and nothing else. |
| Read | The project's `input/` directory. |
| Read, write, create, remove | The run's staging directory: regular files and directories only. |
| Network | None. |
| New processes | None. `fork`, `vfork`, `clone`, and `clone3` fail with `EPERM`; no other program can be executed. |
| Signals | Only to itself, so `abort()` still works. |

It follows that a generated program cannot read the project source, the generated C, assembly, or binary of other builds, run records, the model cache, credentials, or any other host file. It cannot modify inputs or compiler-owned artifacts. It cannot create symbolic links, hard links to inputs, devices, sockets, or pipes in staging.

The process starts with no arguments, `/dev/null` as standard input, an environment of exactly `LC_ALL=C` and `TZ=UTC`, and no descriptors other than 0 to 4.

### Build sandbox: gcc and g++

Malicious C can try to read host files through `#include` or exhaust the compiler, so the compiler is sandboxed too.

| Access | Granted |
| --- | --- |
| Read and execute | `/usr`, `/bin`, `/lib`, `/lib64`, and the compiler's own prefix. |
| Read | `/etc/ld.so.cache`, `/etc/alternatives`, the seqc runtime header and library, the Google Test prefix if one is configured. |
| Read and write | The attempt's working directory, and `/dev/null`. |
| Network | None. |
| New processes | Allowed: the compiler driver starts `cc1`, `as`, and `ld`. |

The compiler gets a clean environment (`PATH`, `LC_ALL`, `TMPDIR` only), so variables such as `CPATH` or `LIBRARY_PATH` cannot redirect it. It cannot read the user's home directory, the rest of `/etc`, `/proc`, the project's inputs, or other attempts.

### Limits

| Limit | Default | Setting |
| --- | --- | --- |
| CPU time | 30 s | `limits.cpu_seconds` |
| Wall-clock time | 60 s | `limits.wall_seconds` |
| Address space | 1024 MiB | `limits.memory_mb` |
| Largest file | 64 MiB | `limits.file_mb` |
| stdout plus stderr | 8 MiB | `limits.output_mb` |
| Total left in staging | 256 MiB | `limits.staging_mb` |
| One compiler invocation | 180 s, 4096 MiB | `build.compile_timeout_seconds`, `build.compile_memory_mb` |

### Other rules

- **Inputs.** `input/` is enumerated without following links. A symbolic link or a special file in it is an error before any inference. Size and count limits apply.
- **Paths.** A plan may only declare relative output paths without `..` and outside `temp/`. Publishing refuses a target whose parent is a link or a file, and never replaces a file `seqc` did not create unless `--force` is given.
- **Downloads.** Only `seqc model pull` downloads, and only the one pinned file, verified against its SHA-256. Nothing from a repository is executed. Model access and generated-program network access are unrelated: generated programs never have a network.
- **Remote inference.** Not available in v0.1. `model.endpoint` must be a loopback address. No project content leaves the machine.
- **Prompt injection.** Input excerpts are delimited and labelled as data in prompts. This is not relied on: whatever the model is talked into writing still runs in the sandbox.
- **Records.** Run directories are created with mode 0700 because they can hold generated text and input excerpts. They are kept until `seqc clean`.
- **Concurrency.** An advisory lock in `output/temp/` makes a second `seqc` in the same project fail at once.

## What was tested

Acceptance of isolation requires real denials, not warnings or keyword scans. `tests/security/` runs a statically linked probe under the same policies `seqc` uses and checks what the kernel let it do. Each denial has a control: the same action without the sandbox succeeds.

| Area | Checked |
| --- | --- |
| Reads | `/etc/passwd`, `/proc/self/environ`, a file elsewhere on the host, directory listings of `/` and the parent of staging: denied. Own files and inputs: allowed. |
| Compiler artifacts | Read, write, truncate, unlink, and create next to them: denied. |
| Inputs | Write, truncate, unlink, rename, create: denied. |
| Writes | Outside staging by absolute and by relative path: denied. |
| Links | Symbolic links in staging, hard links to inputs: denied. |
| Network | TCP, UDP, and Unix sockets: denied. |
| Processes | `fork`, executing `/bin/true` and `/bin/sh`: denied. |
| Signals | To the supervisor: denied. To itself: allowed. |
| Limits | CPU, wall clock, output size, file size, address space: enforced. |
| Environment | Arguments, environment, working directory, standard input, descriptors: exactly as specified, with nothing inherited. |
| Build sandbox | Subprocesses and reads of the system toolchain allowed; network, `/etc/passwd`, `/proc`, the user's files, inputs, and writes outside the working directory denied. |
| Process group | A child left behind by an exiting program is killed. |

`tests/integration/failures.sh` repeats the main denials through the whole pipeline, with a generated program that calls plain `fopen`, and runs the same binary unsandboxed as a control.

### Environments

| Environment | Result |
| --- | --- |
| WSL2 on Windows 11, Ubuntu 24.04 userland, kernel 6.18 (Landlock ABI 7), GCC 13.3, on the Linux filesystem (ext4) | All suites pass. This is the development environment. |
| The same, with the build tree and projects on a Windows drive (`/mnt/d`, a 9p mount) | All suites pass, several times slower. See "Windows drives under WSL" below. |
| Ubuntu 24.04 LTS on bare metal or a VM, stock kernel 6.8 (Landlock ABI 4) | **Not yet run.** The code handles ABI 3 and newer; see the note below. |
| GitHub-hosted `ubuntu-24.04` runner | **Not yet run.** `.github/workflows/ci.yml` runs the suites there on the first push. |

The plan requires all three before Phase 1 closes. Only the first has been proven.

### Windows drives under WSL

A Landlock rule is attached to the kernel's in-memory object for a file or directory. The 9p filesystem that WSL uses for Windows drives discards that object as soon as its last user closes it, so a rule added from a descriptor that is then closed stops matching: the next lookup of the same path produces a new object, and access is denied. The first version of the sandbox did exactly that, and every sandboxed program on `/mnt/d` failed to start with "Permission denied".

The supervisor now opens every granted path itself and holds those descriptors until the child has exited. The child adds its rules from the same descriptors. That keeps the objects, and so the rules, alive. The failure mode of this arrangement is a denial, never extra access: an object that no rule matches is always refused.

It works, and the whole suite passes there, but a Windows drive is slow for this workload. Keep build trees and projects on the Linux filesystem, for example under `~`, when you can.

## Requirements and known limits

- **Kernel.** Landlock ABI 3 or newer, which means Linux 6.2 or newer with Landlock enabled. ABI 3 added control over truncation; without it a program could truncate its read-only inputs. `seqc doctor` reports the ABI and refuses older kernels.
- **Older ABIs get weaker extras.** TCP rules need ABI 4 and signal scoping needs ABI 6. The seccomp filter denies all socket creation and all signals to other processes on every supported kernel, so these Landlock features are a second layer where present, not a requirement.
- **x86_64 only.** The seccomp filter accepts only the native x86_64 system call ABI.
- **No process-count limit for the compiler.** `RLIMIT_NPROC` counts all of a user's processes, so it cannot bound one sandbox. The build sandbox is bounded by time and memory per process, and the whole group is killed at the end. Generated programs cannot create processes at all.
- **Address-space limit, not resident memory.** `limits.memory_mb` bounds virtual address space. A program that reserves a lot and touches little is stopped earlier than its real use would suggest.
- **Hygiene checks are not security.** The scan for names such as `system` is there to keep generated code inside the runtime contract and to give the model a clear repair message. It can be bypassed in ways the sandbox cannot.
- **Other network and FUSE filesystems are untested.** Only ext4 and WSL's 9p mounts were tried. On a filesystem where rules do not hold, programs are denied access; they are not given more.
- **Kernel bugs.** A sandbox escape through a kernel vulnerability is outside what `seqc` can prevent. Run it as an unprivileged user.
- **Outputs are untrusted data.** A published file is whatever the program wrote. Kind checks are structural only: a PNG signature for images, valid UTF-8 for text.
