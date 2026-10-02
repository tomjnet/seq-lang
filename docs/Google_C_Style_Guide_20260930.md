# Google-Style C Style Guide

**Version:** 2026-09-30
**Scope:** C source code (C11/C17, with C23 features where noted)

> Google publishes an official *C++* Style Guide but no official C guide. This document adapts
> the principles and conventions of the Google C++ Style Guide to plain C. Where a C++ rule does
> not apply to C (classes, exceptions, templates, namespaces, RTTI), it is replaced by the
> nearest C equivalent. Rules marked **[C adaptation]** are not in the C++ guide.

---

## Table of Contents

1. [Guiding Principles](#1-guiding-principles)
2. [Language Version](#2-language-version)
3. [Header Files](#3-header-files)
4. [Scoping and Linkage](#4-scoping-and-linkage)
5. [Structs, Types, and Data](#5-structs-types-and-data)
6. [Functions](#6-functions)
7. [Memory and Resource Management](#7-memory-and-resource-management)
8. [Error Handling](#8-error-handling)
9. [Other C Features](#9-other-c-features)
10. [Preprocessor and Macros](#10-preprocessor-and-macros)
11. [Naming](#11-naming)
12. [Comments](#12-comments)
13. [Formatting](#13-formatting)
14. [Testing](#14-testing)
15. [Tooling](#15-tooling)
16. [Exceptions to the Rules](#16-exceptions-to-the-rules)
17. [Quick Reference](#17-quick-reference)

---

## 1. Guiding Principles

- **Optimize for the reader, not the writer.** Code is read far more often than it is written.
- **Be consistent with existing code.** Local consistency beats personal preference.
- **Avoid surprising or dangerous constructs.** Prefer the obvious way over the clever way.
- **Rules should pull their weight.** A rule exists only when its benefit outweighs the cost of
  remembering it.
- **Be consistent with the broader C community** when it doesn't conflict with the above.
- **Concede to optimization when necessary**, but document why.

---

## 2. Language Version

- Target **C17** (ISO/IEC 9899:2018) by default. C11 is acceptable where toolchains require it.
- C23 features (`nullptr`, `bool` as a keyword, `[[nodiscard]]`, `constexpr` objects,
  `typeof`, `#embed`) may be used only when every supported compiler handles them; gate them
  behind a feature macro otherwise.
- Do not use compiler-specific extensions (`__attribute__`, statement expressions, nested
  functions, `__builtin_*`) outside of a small, clearly named portability header such as
  `base/compiler.h`.
- Compile with warnings enabled and treated as errors (see [Tooling](#15-tooling)).

---

## 3. Header Files

Every `.c` file should normally have an associated `.h` file. Exceptions: files containing
only `main()` and unit tests.

### 3.1 Self-contained Headers

Headers must compile on their own: each header includes everything it needs, and nothing
relies on an include order in the `.c` file. Header files end in `.h`. Non-header files
meant for textual inclusion (rare) end in `.inc`.

### 3.2 Include Guards

Every header uses a `#define` guard derived from the full project path:
`<PROJECT>_<PATH>_<FILE>_H_`.

```c
// File: foo/src/bar/baz.h
#ifndef FOO_BAR_BAZ_H_
#define FOO_BAR_BAZ_H_

...

#endif  // FOO_BAR_BAZ_H_
```

`#pragma once` is acceptable only if the project explicitly standardizes on it.

### 3.3 Include What You Use

If a file uses a symbol, include the header that declares it directly. Do not rely on
transitive includes.

### 3.4 Forward Declarations

Prefer including the header. Forward-declaring an **opaque struct** (`struct Foo;`) is
acceptable and encouraged when a header only passes pointers to it, because it reduces
coupling and rebuild time.

### 3.5 Inline Functions

Define `static inline` functions in headers only when they are short (roughly 10 lines or
fewer) and performance-relevant. Never put non-`static` function definitions in headers.

### 3.6 Include Order

Separate each group with a blank line, and sort alphabetically within groups:

1. The related header (`foo/server/fooserver.h` in `fooserver.c`)
2. C standard library headers (`<stdio.h>`, `<stdlib.h>`)
3. POSIX / system headers (`<unistd.h>`, `<sys/types.h>`)
4. Third-party library headers
5. Your project's headers

```c
#include "foo/server/fooserver.h"

#include <stdint.h>
#include <string.h>

#include <sys/types.h>
#include <unistd.h>

#include "third_party/zlib/zlib.h"

#include "foo/base/logging.h"
#include "foo/server/bar.h"
```

Project headers are written with paths relative to the project root; never use `.` or `..`.

---

## 4. Scoping and Linkage

### 4.1 Internal Linkage **[C adaptation]**

C has no namespaces. Any function or file-scope variable not used outside its translation unit
**must** be declared `static`. This replaces the C++ "unnamed namespace" rule.

```c
static int ComputeChecksum(const uint8_t* data, size_t len);
static const char kDefaultName[] = "unnamed";
```

### 4.2 Symbol Prefixes **[C adaptation]**

All externally visible symbols (functions, types, global variables, enum constants, macros)
carry a short project/module prefix to avoid collisions in the single global namespace.

```c
// Module "seq" (the library), sub-module "buf".
typedef struct SeqBuffer SeqBuffer;
SeqBuffer* seq_buffer_create(size_t capacity);
#define SEQ_BUFFER_MAX_CAPACITY 4096
```

### 4.3 Local Variables

- Declare variables in the narrowest scope possible, as close to first use as possible.
- Initialize variables at declaration.
- Declare loop counters inside the `for` statement.

```c
for (size_t i = 0; i < n; ++i) { ... }   // Good.

int i;                                    // Bad: wide scope, uninitialized.
...
for (i = 0; i < n; ++i) { ... }
```

### 4.4 Global and Static Variables

- Mutable globals are strongly discouraged. Prefer passing a context struct.
- File-scope `static` variables are permitted for constants and for well-encapsulated module
  state, but must be documented and must be thread-safe if the module can be used
  concurrently.
- Constants with static storage should be `static const` (or `constexpr` in C23).
- Function-local `static` variables are allowed for caching but make the function
  non-reentrant — document it.

### 4.5 Thread-local Variables

Use `_Thread_local` (or `thread_local` from `<threads.h>` / C23) only when necessary, and
document ownership and lifetime.

---

## 5. Structs, Types, and Data

### 5.1 Opaque Types **[C adaptation]**

Encapsulate implementation details by exposing an incomplete type in the header and defining
it in the `.c` file. This is the C equivalent of private class members.

```c
// seq_buffer.h
typedef struct SeqBuffer SeqBuffer;
SeqBuffer* seq_buffer_create(size_t capacity);
void seq_buffer_destroy(SeqBuffer* buf);
size_t seq_buffer_size(const SeqBuffer* buf);

// seq_buffer.c
struct SeqBuffer {
  uint8_t* data;
  size_t size;
  size_t capacity;
};
```

### 5.2 Transparent Structs

Use a transparent (fully defined in the header) struct only for passive data carriers with no
invariants between fields, e.g. `SeqPoint { int x; int y; }`.

### 5.3 typedefs

- `typedef` structs and enums to a `PascalCase` name.
- Do **not** `typedef` pointer types (`typedef struct Foo* FooPtr;` hides indirection).
- Function-pointer typedefs are encouraged for readability.

```c
typedef void (*SeqLogCallback)(void* user_data, const char* msg);
```

### 5.4 Integer Types

- Use `int` for small values where size doesn't matter (loop counters, flags).
- Use `<stdint.h>` fixed-width types (`int32_t`, `uint64_t`, …) when size matters, including
  on-disk/wire formats.
- Use `size_t` for object sizes and array indexes; `ptrdiff_t` for pointer differences.
- Do not use `short`, `long`, or `long long` for sizing — use fixed-width types.
- Avoid unsigned types merely to assert non-negativity; mixing signed and unsigned in
  arithmetic is a common bug source. Use unsigned for bit patterns and modular arithmetic.
- Use `bool` from `<stdbool.h>` (or the C23 keyword) for booleans.

### 5.5 Enums

- Give every enum a named type and prefixed constants.
- When switching over an enum, handle every value and omit `default` so the compiler can warn
  about missing cases (or add a `default` that asserts).

```c
typedef enum {
  kSeqColorRed,
  kSeqColorGreen,
  kSeqColorBlue,
} SeqColor;
```

### 5.6 Designated Initializers

Prefer designated initializers for structs and sparse arrays.

```c
SeqOptions opts = {
    .capacity = 128,
    .flags = kSeqFlagNone,
};
```

### 5.7 Floating Point

Use `double` by default; use `float` only when memory or SIMD layout requires it. Never compare
floats for exact equality except with 0 or known-exact values.

---

## 6. Functions

### 6.1 Inputs and Outputs

- Prefer return values over output parameters.
- Inputs come before outputs in the parameter list.
- Input pointers that are not modified are `const`-qualified.
- Output parameters are non-`const` pointers and must be documented as outputs.

```c
// Parses `text` and stores the result in `*out`. Returns false on error.
bool seq_parse_int(const char* text, int64_t* out);
```

### 6.2 Function Length

Prefer small, focused functions. If a function exceeds roughly 40 lines, consider whether it
can be split without harming readability.

### 6.3 Parameters

- Use `void` for an empty parameter list in declarations: `int seq_version(void);`
  (not needed in C23, but harmless and still required for C17).
- Pass arrays with an explicit length parameter. Never assume a sentinel unless it is
  documented (e.g. NUL-terminated strings).
- Pass structs larger than two machine words by `const` pointer.

### 6.4 Variadic Functions

Avoid defining new variadic functions except for printf-style wrappers. Mark those with a
format-checking attribute via the portability header.

### 6.5 Recursion

Avoid unbounded recursion; stack space is finite and not checked. Prefer iteration for data
of unbounded depth.

---

## 7. Memory and Resource Management **[C adaptation]**

### 7.1 Ownership

Every heap allocation has exactly one clearly documented owner. Function comments state
whether a pointer argument is **borrowed** or **ownership is transferred**, and whether a
returned pointer must be freed by the caller.

### 7.2 Create/Destroy Pairs

Every type that allocates provides a matching pair: `xxx_create()` / `xxx_destroy()` or
`xxx_init()` / `xxx_deinit()`. `destroy` functions accept `NULL` and do nothing.

### 7.3 Allocation Rules

- Always check allocation results.
- Guard size computations against overflow before calling `malloc` (use `calloc` or a checked
  multiply helper).
- Set pointers to `NULL` after freeing when the variable remains in scope.
- Never `free` memory you didn't allocate through the matching allocator.

```c
SeqBuffer* seq_buffer_create(size_t capacity) {
  SeqBuffer* buf = calloc(1, sizeof(*buf));
  if (buf == NULL) return NULL;
  buf->data = calloc(capacity, sizeof(*buf->data));
  if (buf->data == NULL) {
    free(buf);
    return NULL;
  }
  buf->capacity = capacity;
  return buf;
}
```

### 7.4 sizeof

Prefer `sizeof(varname)` / `sizeof(*ptr)` over `sizeof(type)`, so the expression stays correct
if the variable's type changes.

### 7.5 Single-exit Cleanup

For functions acquiring multiple resources, a `goto cleanup` pattern with a single exit is
permitted and preferred over deeply nested `if`s. `goto` is not permitted for any other
purpose.

```c
int seq_load(const char* path) {
  int rc = -1;
  FILE* f = NULL;
  char* buf = NULL;

  f = fopen(path, "rb");
  if (f == NULL) goto cleanup;
  buf = malloc(kChunkSize);
  if (buf == NULL) goto cleanup;
  ...
  rc = 0;

cleanup:
  free(buf);
  if (f != NULL) fclose(f);
  return rc;
}
```

### 7.6 Unsafe Library Functions

Never use `gets`, `strcpy`, `strcat`, `sprintf`, or `atoi`. Use bounded alternatives:
`fgets`, `snprintf`, `strtol`/`strtoll` with error checking, and explicit-length copies.

---

## 8. Error Handling **[C adaptation]**

C has no exceptions. Choose one error-reporting convention per module and use it consistently:

1. **Status enum** (preferred for libraries): return a `SeqStatus`, with results via output
   parameters.
2. **bool** for simple success/failure where the reason is irrelevant.
3. **NULL** from constructors/lookups that can fail.

```c
typedef enum {
  kSeqOk = 0,
  kSeqErrInvalidArgument,
  kSeqErrOutOfMemory,
  kSeqErrIo,
} SeqStatus;
```

- Never ignore returned errors. Mark functions whose results must be checked with a
  `SEQ_MUST_USE` macro (mapping to `[[nodiscard]]` or `__attribute__((warn_unused_result))`).
- Use `assert()` for programmer errors (invariant violations), not for runtime conditions
  such as bad input or I/O failure.
- `errno` is read immediately after the failing call, before any other library call.
- Do not use `setjmp`/`longjmp` for error handling.

---

## 9. Other C Features

### 9.1 const

Use `const` wherever it is correct: pointer-to-const parameters, constant data, and local
variables that don't change. Place `const` before the type: `const char* name`.

### 9.2 Casts

- Avoid casts; when necessary, keep them local and explain non-obvious ones.
- Do not cast the result of `malloc` (it hides a missing `#include <stdlib.h>` in older C).
- Do not cast away `const`.

### 9.3 Pointers and NULL

- Use `NULL` (or `nullptr` in C23 code) for null pointers, never `0`.
- Compare explicitly: `if (ptr == NULL)` is preferred; `if (!ptr)` is acceptable if used
  consistently.

### 9.4 Variable-length Arrays

Do not use VLAs. They risk stack overflow on untrusted sizes and are optional in C11.

### 9.5 Preincrement

Prefer `++i` for standalone increments; it states intent and matches C++ codebases.

### 9.6 Boolean Expressions and Conditions

- Do not use assignment in a condition unless idiomatic (`while ((c = getc(f)) != EOF)`), and
  wrap it in an extra set of parentheses.
- Use parentheses to clarify mixed `&&`/`||` and bitwise operators.

### 9.7 Static Assertions

Use `_Static_assert` (or `static_assert`) to verify compile-time assumptions such as struct
sizes and enum counts.

### 9.8 Portability

- Do not assume `sizeof(int) == 4`, `sizeof(long) == 8`, or pointer sizes.
- Use `PRId64`, `PRIu64`, `%zu` etc. for printing fixed-width and `size_t` values.
- Be explicit about byte order in serialized data.

### 9.9 Concurrency

Use `<stdatomic.h>` and `<threads.h>` (or a project wrapper around pthreads). Document which
functions are thread-safe. Any shared mutable state is protected by a lock or accessed only
via atomics.

---

## 10. Preprocessor and Macros

- Prefer `static inline` functions, `enum` constants, and `static const` variables over macros.
- When macros are necessary:
  - Name them `ALL_CAPS` with the project prefix.
  - Parenthesize every parameter use and the whole expression.
  - Wrap multi-statement macros in `do { ... } while (0)`.
  - Never evaluate an argument more than once if it could have side effects.
  - `#undef` macros used only locally after use.
- Do not use macros to define pieces of the language (e.g. `#define BEGIN {`).
- Keep `#if` nesting shallow; comment the matching `#endif`.
- Prefer `#if defined(X)` over `#ifdef X` when combining conditions.

```c
#define SEQ_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define SEQ_CHECK(cond)                                   \
  do {                                                    \
    if (!(cond)) seq_fatal(__FILE__, __LINE__, #cond);    \
  } while (0)
```

---

## 11. Naming

Names should be descriptive; avoid abbreviations unfamiliar outside your project. Longer scope
warrants a longer, more descriptive name.

| Entity                          | Convention                    | Example                        |
|---------------------------------|-------------------------------|--------------------------------|
| Files                           | `lower_snake_case.c` / `.h`   | `seq_buffer.c`                 |
| Types (struct/enum/union/typedef)| `PascalCase` + prefix        | `SeqBuffer`, `SeqStatus`       |
| Public functions **[C adaptation]** | `prefix_module_verb`      | `seq_buffer_append()`          |
| Static (file-local) functions   | `PascalCase` or `snake_case`, consistent per project | `ComputeHash()` |
| Local variables & parameters    | `lower_snake_case`            | `table_name`, `num_rows`       |
| Struct members                  | `lower_snake_case` (no trailing `_` needed in C) | `capacity` |
| Global variables (discouraged)  | `g_` + `lower_snake_case`     | `g_seq_log_level`              |
| Constants (`static const`, enum values) | `k` + `PascalCase`    | `kMaxBufferSize`, `kSeqOk`     |
| Macros                          | `PREFIX_ALL_CAPS`             | `SEQ_ARRAY_SIZE`               |
| Include guards                  | `PATH_FILE_H_`                | `SEQ_CORE_BUFFER_H_`           |

**Notes**

- The C++ guide uses `PascalCase` for all functions. In C, `snake_case` with a module prefix
  for public API is the overwhelming convention (matching the standard library and POSIX), so
  this guide adopts it for public functions. Pick one style for file-local functions and use
  it throughout the project.
- Never begin names with an underscore followed by an uppercase letter or with a double
  underscore — these are reserved for the implementation. Avoid the `_t` suffix for your own
  types; POSIX reserves it.

---

## 12. Comments

Comments are essential, but the best code is self-documenting. Use `//` or `/* */` consistently;
`//` is preferred for C99 and later.

### 12.1 File Comments

Begin each file with license boilerplate (if applicable) and, for headers, a short description
of the module's purpose and usage.

### 12.2 Type Comments

Every non-obvious struct or enum declaration gets a comment describing what it represents,
invariants between fields, and thread-safety.

### 12.3 Function Comments

Declaration comments (in the header) describe **what** the function does and how to use it:

- Inputs and outputs, and which pointers may be `NULL`.
- Ownership of pointer arguments and return values.
- Error conditions and return values.
- Thread-safety and reentrancy, if relevant.
- Performance characteristics, if non-obvious.

Definition comments (in the `.c` file) describe **how**, when the implementation is tricky.

```c
// Appends `len` bytes from `data` to `buf`, growing it if needed.
// `data` is borrowed and may be NULL only if `len` is 0.
// Returns kSeqErrOutOfMemory if the buffer cannot grow; `buf` is unchanged in that case.
// Not thread-safe.
SeqStatus seq_buffer_append(SeqBuffer* buf, const uint8_t* data, size_t len);
```

### 12.4 Implementation Comments

Comment tricky, non-obvious, or important code. Explain *why*, not *what*. Do not restate
code.

### 12.5 Argument Comments

When a literal argument's meaning is unclear, prefer a named constant or enum. Otherwise use
an inline comment:

```c
seq_open(path, /*create_if_missing=*/true);
```

### 12.6 TODO Comments

Use `TODO` with a bug/issue reference or owner, and a description:

```c
// TODO(bug 12345): Replace linear scan with a hash table once inputs exceed 10k.
```

### 12.7 Punctuation and Grammar

Write comments as complete sentences with proper capitalization and punctuation. Short
end-of-line comments may be fragments.

---

## 13. Formatting

Automate formatting with `clang-format` using `BasedOnStyle: Google` (see
[Tooling](#15-tooling)). The rules below describe the result.

### 13.1 Line Length

Maximum **80 characters**. Exceptions: long URLs in comments, `#include` lines, include guards,
and string literals that cannot be sensibly split.

### 13.2 Indentation

- **2 spaces** per level. No tabs.
- Continuation lines indent 4 spaces, or align with the opening parenthesis.

### 13.3 Braces

Opening brace on the same line as the statement or function signature.

```c
int seq_max(int a, int b) {
  return a > b ? a : b;
}
```

### 13.4 Function Declarations and Calls

Return type on the same line as the name when it fits; otherwise wrap parameters.

```c
SeqStatus seq_encode_frame(const SeqEncoder* encoder, const SeqFrame* frame,
                           uint8_t* out, size_t out_len);

SeqStatus seq_encode_frame_with_really_long_name(
    const SeqEncoder* encoder, const SeqFrame* frame, uint8_t* out,
    size_t out_len);
```

### 13.5 Conditionals

- Space between `if` and `(`; no spaces inside the parentheses.
- `else` on the same line as the closing brace.
- Braces are required unless the entire statement fits on one line with no `else`.

```c
if (condition) {
  DoOneThing();
} else if (other) {
  DoAnotherThing();
} else {
  DoNothing();
}

if (x == NULL) return kSeqErrInvalidArgument;   // OK: single line.
```

### 13.6 Loops and Switch

```c
switch (color) {
  case kSeqColorRed: {
    HandleRed();
    break;
  }
  case kSeqColorGreen:
    HandleGreen();
    [[fallthrough]];   // Or a /* fallthrough */ comment before C23.
  case kSeqColorBlue:
    HandleBlueish();
    break;
}

while (condition) {
  // Empty loop bodies use {} or `continue;`, never a lone `;`.
}
```

### 13.7 Pointer Declarations

Attach `*` to the type, and declare one pointer per line.

```c
char* name = NULL;        // Good.
const SeqBuffer* buf;     // Good.
char *a, *b;              // Bad: multiple declarators.
```

### 13.8 Operators and Expressions

- Spaces around binary and ternary operators; none after unary operators.
- Do not add spaces inside parentheses.
- Wrap long boolean expressions with the operator at the end of the line.

```c
if (this_one_thing > this_other_thing &&
    a_third_thing == a_fourth_thing) {
  ...
}
```

### 13.9 Return Values

Do not surround the return expression with parentheses unnecessarily: `return result;`.

### 13.10 Preprocessor Directives

`#` always starts at column 0, even inside indented code. Nested directives may indent after
the `#`.

```c
#if defined(SEQ_ENABLE_SIMD)
#  include <immintrin.h>
#endif
```

### 13.11 Horizontal and Vertical Whitespace

- No trailing whitespace.
- Minimize blank lines; don't start or end a block with a blank line.
- One blank line between function definitions.
- Files end with a single newline.

---

## 14. Testing

- Every public function has unit tests. Use a lightweight C test framework (e.g. Unity,
  cmocka, or GoogleTest compiled as C++ against the C headers).
- Test files are named `<module>_test.c`.
- Run tests under AddressSanitizer and UndefinedBehaviorSanitizer in CI; run concurrency tests
  under ThreadSanitizer.
- Fuzz any function that parses untrusted input (libFuzzer or AFL++).

---

## 15. Tooling

### 15.1 Recommended `.clang-format`

```yaml
BasedOnStyle: Google
Language: Cpp          # clang-format uses the Cpp language mode for C.
ColumnLimit: 80
IndentWidth: 2
DerivePointerAlignment: false
PointerAlignment: Left
IncludeBlocks: Regroup
SortIncludes: CaseSensitive
AllowShortIfStatementsOnASingleLine: WithoutElse
AllowShortFunctionsOnASingleLine: Empty
```

### 15.2 Compiler Flags

```text
-std=c17 -Wall -Wextra -Wpedantic -Werror
-Wshadow -Wconversion -Wsign-conversion -Wstrict-prototypes
-Wmissing-prototypes -Wformat=2 -Wundef -Wcast-qual -Wvla
-Wimplicit-fallthrough -Wnull-dereference
```

MSVC: `/W4 /WX /std:c17 /permissive-`.

### 15.3 Static Analysis

Run `clang-tidy` with at least the `bugprone-*`, `cert-*`, `clang-analyzer-*`, `misc-*`,
`readability-*`, and `performance-*` checks, tuning noisy ones per project.

---

## 16. Exceptions to the Rules

- **Existing non-conforming code:** follow the local conventions of the file you are editing
  rather than mixing styles. Reformat only in dedicated, behavior-neutral changes.
- **Third-party code:** leave vendored code in its original style.
- **Platform APIs:** when wrapping Windows or POSIX APIs, matching their naming inside the thin
  wrapper layer is acceptable.

When in doubt, **be consistent**. If code you add looks drastically different from the code
around it, the discontinuity distracts readers from what matters.

---

## 17. Quick Reference

| Topic              | Rule                                                        |
|--------------------|-------------------------------------------------------------|
| Standard           | C17; C23 features only when all toolchains support them     |
| Indentation        | 2 spaces, no tabs                                           |
| Line length        | 80 columns                                                  |
| Braces             | Same line (K&R/Google)                                      |
| Pointer style      | `Type* name`, one declarator per line                       |
| Header guards      | `PROJECT_PATH_FILE_H_`                                      |
| File-local symbols | Always `static`                                             |
| Public symbols     | Module prefix (`seq_…`, `Seq…`, `SEQ_…`)                    |
| Types              | `PascalCase` typedefs; never typedef pointers               |
| Constants          | `kPascalCase`                                               |
| Macros             | `PREFIX_ALL_CAPS`; prefer inline functions/enums            |
| Errors             | Status enum / bool / NULL — consistent per module; check all |
| Memory             | Documented ownership; `create`/`destroy` pairs; NULL-safe destroy |
| Cleanup            | `goto cleanup` allowed only for single-exit resource release |
| Forbidden          | VLAs, `gets`/`strcpy`/`sprintf`/`atoi`, `setjmp`/`longjmp` for errors |
| Formatting tool    | `clang-format` with `BasedOnStyle: Google`                  |

---

*Adapted from the principles of the Google C++ Style Guide
(https://google.github.io/styleguide/cppguide.html) for use with C.*
