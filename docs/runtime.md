# The runtime contract

**Runtime version:** 1

A generated program is one C17 translation unit with three parts. The model writes only the middle one.

```c
/* ---- Compiler-owned prelude ---- */
#include "seq_runtime.h"
#include <ctype.h>   /* errno float inttypes limits math stdbool stddef   */
...                  /* stdint stdio stdlib string                        */

int seq_step_1(seq_ctx *ctx);
int seq_step_2(seq_ctx *ctx);

/* ---- Step functions (model-written) ---- */
int seq_step_1(seq_ctx *ctx) { ... }
int seq_step_2(seq_ctx *ctx) { ... }

/* ---- Compiler-owned driver ---- */
#ifndef SEQ_NO_MAIN
static const seq_step_entry seq_steps[] = {
    {"step1", seq_step_1},
    {"step2", seq_step_2},
};

int main(void) {
  return seq_run(seq_steps, 2);
}
#endif /* SEQ_NO_MAIN */
```

Step functions are named by 1-based source order, so a step name chosen by the user can never collide with a C identifier. Steps share data through file-scope `static` variables or through files.

## Runtime API

The header `runtime/include/seq_runtime.h` is the single source of the API. Its text is part of the generation prompt, so it is kept small.

| Function | Purpose |
| --- | --- |
| `FILE* seq_output_open(const char* relpath, const char* mode)` | Opens a file in the output directory. Rejects an absolute path, a `..` or `.` component, an empty component, and a backslash. Creates missing parent directories when writing. |
| `FILE* seq_input_open(const char* relpath)` | Opens a file from the project's `input/` directory, read-only, with the same path rules. |
| `int seq_fail(seq_ctx* ctx, const char* fmt, ...)` | Records a message for the running step and returns 1, for `return seq_fail(ctx, "...")`. |
| `int seq_chart_bar_png(const char* relpath, const char* title, const char* const* labels, const double* values, int count)` | Draws a labelled bar chart as an 800 by 480 PNG. `count` is 1 to 32. Returns 0 on success. |
| `void seq_ctx_init(seq_ctx* ctx)` | Prepares a context for calling a step directly. Used by tests. |
| `int seq_run(const seq_step_entry* steps, int count)` | The driver. Used only by the compiler-owned `main`. |

The path helpers are a convenience. The sandbox is the enforcement: a program that ignores the helpers and calls `fopen("/etc/passwd", "r")` gets `NULL`.

## Driver and process environment

The supervisor (`src/execution/`) starts the program with:

| Item | Value |
| --- | --- |
| Working directory | The staging directory of the run. |
| Arguments | None. |
| Standard input | `/dev/null`. |
| Environment | Exactly `LC_ALL=C` and `TZ=UTC`. |
| Descriptor 3 | A pipe for step records. |
| Descriptor 4 | The `input/` directory, opened read-only, when the project has one. |
| Other descriptors | None. |

Standard output and standard error stay free for the workflow's own output.

`seq_run` calls the steps in order. Before a step it writes `begin <index> <name>`. After it, `ok <index> <name>`; or, if the step returned nonzero, `message <index> <text>` when the step called `seq_fail`, then `fail <index> <name> <code>`, and it stops. One record per line: newlines in a message become spaces.

| Outcome | Exit status |
| --- | --- |
| Every step returned 0 | 0 |
| A step returned nonzero | 70 (`SEQ_EXIT_STEP_FAILED`) |
| Killed by a signal or a limit | Reported by the supervisor, with the step that was running |

The runtime looks at descriptors 3 and 4 once, at its first call. If descriptor 3 is not a pipe, as when the program is run by hand, no records are written and a failure is printed to standard error instead. If descriptor 4 is not a directory, inputs are resolved under `$SEQ_INPUT_DIR`, or `input/` in the working directory.

## Chart helper

The helper draws into an 8-bit palette canvas and writes it with an encoder written for this project: one fixed-Huffman deflate block whose matches are runs of a repeated byte. A chart is mostly flat color: a three-bar chart is under 10 KiB, against 384 KiB of raw pixels. Text uses a 5 by 7 bitmap font drawn for this project (`runtime/src/seq_font.c`), stored as readable art in the source. Characters outside printable ASCII are drawn as `?`. Labels that do not fit are drawn smaller, then truncated.

The value axis starts at zero and extends to a rounded tick above the largest value. Negative values are drawn below the zero line.

The test suite decodes the output with Python's `zlib`, which is independent of this encoder.

## Hygiene checks

Before the compiler sees model-written code, `src/backends/c/c_backend.cpp` scans it. These checks keep generated code inside the contract. They are not a security boundary; [security.md](security.md) describes that.

| Rule | Detail |
| --- | --- |
| Includes | Only C standard headers from an allowlist, and `seq_runtime.h`. No paths. Headers for signals, non-local jumps, and threads are not on the list. |
| Directives | No `#include_next`, `#embed`, `#import`, `#pragma`, or `#line`. Conditionals must balance. |
| `main` | Must not be defined or referenced. |
| Step functions | Each required `seq_step_<n>` defined exactly once; none beyond the number of steps. |
| Forbidden names | `system`, `popen`, `fork`, `vfork`, the `exec*` family, `posix_spawn*`, `socket`, `socketpair`, `dlopen`, `dlsym`, `syscall`, `ptrace`, `asm`, `__asm__`, `SEQ_NO_MAIN`. |
| Forbidden calls | `connect`, `bind`, `listen`, `accept`, `clone`, `kill`, `signal`, `sigaction`. These common words are allowed as variable names. |
| Structure | Braces must balance. No trigraphs, digraphs, or token pasting, which could hide a name from the scan. Line splices are joined before scanning. |
| Size | At most 256 KiB, valid UTF-8. |

Comments and string literals are skipped. A finding is reported with its line and sent to the model as a repair request.

## Generated tests

Every accepted build has `output/temp/test/<name>_test.cc` and a test binary built from it.

```cpp
// ---- Compiler-owned prelude ----
#include <gtest/gtest.h>
#include <cmath>     // cstdio cstdlib cstring fstream sstream string vector

extern "C" {
#include "seq_runtime.h"
int seq_step_1(seq_ctx *ctx);
}

// ---- Test cases (model-written) ----
TEST(Hello, StepSucceeds) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  EXPECT_EQ(0, seq_step_1(&ctx));
}
```

```sh
gcc -std=c17 -O3 -DSEQ_NO_MAIN -I<home>/include -c <name>.c -o <name>.o
g++ -std=c++17 -I<home>/include <name>_test.cc <name>.o -o <name>_test.bin \
    -static -L<home>/lib -lseqrt -lgtest_main -lgtest -lpthread -lm
```

- The cases are written in a separate inference call that sees the workflow, the plan, and the accepted step functions.
- The same hygiene checks apply, with C++ headers and `gtest/gtest.h` on the allowlist. Death tests are rejected: they need to start a process, which the sandbox denies.
- A test file that does not compile goes through bounded repair with its own attempt count. A build with no compiled test file is not accepted.
- The test binary is untrusted. It runs under the same isolation and limits as the workflow executable, in its own staging directory, before the workflow runs.
- Results per case are shown and recorded in `build.json`. A failing case does not block execution: a model-written test of model-written code can be wrong in either direction. A test binary that crashes is recorded as such.
- On a cache hit the test file and its recorded results are reused. Nothing is inferred or run.
- Google Test is never linked into the workflow executable.

`-lgtest_main` comes before `-lgtest` because static archives resolve left to right.

## Building generated code by hand

`output/temp/Makefile` is an unchanged copy of `docs/Makefile.template`. It builds every `*.c` beside it into `build/<name>.bin`. It does not know where the seqc runtime is, so pass the paths:

```sh
make -C output/temp \
  INCLUDE_FLAGS="-I$SEQC_HOME/include" \
  LINK_FLAGS="-static -L$SEQC_HOME/lib" \
  LINK_LIBS="-lseqrt -lm"
```

`$SEQC_HOME` is the directory `seqc doctor` prints on its `runtime` line. A program built this way runs outside the sandbox, with your own permissions.

## Style

Project-owned C in `runtime/` follows [Google_C_Style_Guide_20260930.md](Google_C_Style_Guide_20260930.md), with two exceptions:

- Public types are lower case (`seq_ctx`, `seq_step_entry`) because the plan fixes the step signature as `int seq_step_1(seq_ctx *ctx)`.
- The glyph table in `seq_font.c` exceeds 80 columns so that each glyph stays on one line.

Generated C is not required to follow the guide in v0.1.
