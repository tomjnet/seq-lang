# Rustfmt Style Guide

**Version:** 2026-09-30
**Scope:** Rust source code (Rust 2024 edition, stable toolchain)

> This guide is based on the official Rust Style Guide as enforced by `rustfmt`, together with
> the Rust API Guidelines and Clippy. Google publishes no official Rust style guide (Rust is
> not among the languages listed at https://google.github.io/styleguide/), so unlike the
> sibling C and C++ guides in this folder it is not a Google guide. Where the community
> standards are silent, it borrows the general principles of the Google C++ Style Guide to
> stay consistent with those siblings. Rules marked **[differs]** intentionally depart from
> the C and C++ guides, because following them would mean fighting the Rust toolchain and
> ecosystem.

---

## Table of Contents

1. [Guiding Principles](#1-guiding-principles)
2. [Language Version and Toolchain](#2-language-version-and-toolchain)
3. [Crates, Modules, and Files](#3-crates-modules-and-files)
4. [Types and Data](#4-types-and-data)
5. [Functions and Methods](#5-functions-and-methods)
6. [Ownership, Borrowing, and Lifetimes](#6-ownership-borrowing-and-lifetimes)
7. [Error Handling](#7-error-handling)
8. [Unsafe Code](#8-unsafe-code)
9. [FFI](#9-ffi)
10. [Traits and Generics](#10-traits-and-generics)
11. [Macros](#11-macros)
12. [Concurrency and Async](#12-concurrency-and-async)
13. [Naming](#13-naming)
14. [Comments and Documentation](#14-comments-and-documentation)
15. [Formatting](#15-formatting)
16. [Testing](#16-testing)
17. [Tooling](#17-tooling)
18. [Exceptions to the Rules](#18-exceptions-to-the-rules)
19. [Quick Reference](#19-quick-reference)

---

## 1. Guiding Principles

- **Optimize for the reader, not the writer.** Code is read far more often than it is written.
- **Be consistent with existing code.** Local consistency beats personal preference.
- **Be consistent with the Rust community.** Rust has one formatter, one linter, and one set
  of naming conventions. Use them unmodified unless a rule here says otherwise.
- **Let the compiler enforce it.** Prefer a design the type system or a lint can check over a
  convention a reviewer has to remember.
- **Avoid surprising or dangerous constructs.** Prefer the obvious way over the clever way.
- **Rules should pull their weight.** A rule exists only when its benefit outweighs the cost of
  remembering it.
- **Concede to optimization when necessary**, but measure first and document why.

---

## 2. Language Version and Toolchain

- Target the **Rust 2024 edition**. Set `edition = "2024"` in every `Cargo.toml`.
- Use the **stable** toolchain only. Do not use nightly features or `#![feature(...)]` in
  production code. Nightly is acceptable for tools that require it (Miri, some sanitizers,
  `cargo fuzz`) as long as the code itself builds on stable.
- Pin the toolchain in `rust-toolchain.toml` and declare the minimum supported Rust version
  with `rust-version` in `Cargo.toml`. Raising either is a deliberate, reviewed change.
- Commit `Cargo.lock` for every crate and workspace, and build with `--locked` in CI.
- Build with warnings treated as errors in CI (see [Tooling](#17-tooling)). Do not write
  `#![deny(warnings)]` in source; it turns every new compiler lint into a build break for
  anyone using a newer toolchain.

```toml
# rust-toolchain.toml
[toolchain]
channel = "1.90.0"   # Example: pin an exact stable release, never "stable" or "nightly".
components = ["rustfmt", "clippy"]
```

---

## 3. Crates, Modules, and Files

### 3.1 Crate Layout

- Follow the standard Cargo layout: `src/lib.rs`, `src/main.rs`, `src/bin/`, `tests/`,
  `benches/`, `examples/`.
- Keep binaries thin. `main.rs` parses arguments, calls into a library crate, and maps the
  result to an exit code. Logic that deserves a test lives in the library.
- Group related crates in a Cargo workspace and share versions, lints, and metadata through
  `[workspace.dependencies]`, `[workspace.lints]`, and `[workspace.package]`.
- Split a crate when it has a separate consumer or a meaningfully separate dependency set,
  not merely because it grew.

### 3.2 Modules and Files

- One module per file. Use `foo.rs` plus a `foo/` directory for submodules; do not create new
  `mod.rs` files.
- A module has one clear purpose. If its `//!` summary needs the word "and", consider
  splitting it.
- Order the contents of a file consistently:
  1. `//!` module documentation
  2. `mod` declarations
  3. `use` declarations
  4. Constants and statics
  5. Types, each followed by its `impl` blocks
  6. Free functions
  7. `#[cfg(test)] mod tests` — always last
- Re-export deliberately. `pub use` defines the public API; do not make callers depend on the
  internal module tree.

### 3.3 Imports

Separate each group with a blank line; `rustfmt` sorts within a group:

1. `std`, `core`, `alloc`
2. External crates
3. The current crate: `crate::`, `super::`, `self::`

```rust
use std::collections::HashMap;
use std::path::{Path, PathBuf};

use serde::Deserialize;

use crate::ast::{Step, Workflow};
use crate::diagnostics::Diagnostic;
```

- Do not use glob imports (`use foo::*`). Exceptions: `use super::*` inside a `tests` module,
  and a crate's explicitly designed `prelude`.
- Import types and traits by name; refer to free functions through their parent module
  (`fs::read_to_string`, not a bare `read_to_string`), so the call site shows where they
  come from.
- When two imported names collide, qualify them by module (`fmt::Result`, `io::Result`)
  rather than renaming with `as`.
- Do not write `extern crate`, except for `extern crate alloc` in `no_std` code.

### 3.4 Visibility

- Everything is private by default. Widen visibility only as far as needed: private, then
  `pub(super)`, then `pub(crate)`, then `pub`.
- In a library crate, `pub` means "part of the supported API". Enable the `unreachable_pub`
  lint so items that are `pub` but not reachable are flagged.
- Struct fields are private unless the struct is passive data with no invariants
  (see [4.1](#41-structs)).

### 3.5 Dependencies

- Every new dependency is a reviewed decision. Check its license, maintenance status,
  transitive dependency count, and use of `unsafe` before adopting it.
- Prefer the standard library, then a small set of widely used crates, then writing the
  code in-house. Do not add a crate to save ten lines.
- Declare versions once in `[workspace.dependencies]`. Disable default features and enable
  only what is used: `default-features = false, features = [...]`.
- Enforce policy with `cargo deny` (licenses, advisories, banned and duplicate crates).
  Use `cargo vet` or an equivalent audit record where supply-chain review is required.
- Builds must not depend on network access beyond fetching locked, checksummed crates.
  Build scripts (`build.rs`) must not download anything.
- Do not use git or path dependencies outside the workspace in released code.

### 3.6 Cargo Features

- Features are **additive**. Enabling a feature must never remove functionality or change
  behavior in a way that breaks another crate that did not ask for it.
- Name features after what they provide (`serde`, `png`), without `use-` or `with-` prefixes.
- Keep `#[cfg(...)]` at the item or module level; avoid scattering it through function
  bodies. Test every supported feature combination in CI.

---

## 4. Types and Data

### 4.1 Structs

- A struct with invariants has private fields, a constructor that establishes the
  invariants, and methods that maintain them.
- A struct that is passive data (a plain record with no invariants) may have public fields.
- Use the **newtype pattern** to distinguish values that share a representation:
  `struct StepIndex(usize);` instead of a bare `usize`.
- For a type with many optional settings, provide a builder or implement `Default` and use
  struct update syntax. Do not write constructors with long positional parameter lists.

### 4.2 Enums

- Prefer an enum over a `bool` or a set of integer constants whenever a value has a closed
  set of states. `Backend::C` is clearer at the call site than `true`.
- Make illegal states unrepresentable: put data in the variant it belongs to rather than in
  `Option` fields that are only valid in some states.
- Match exhaustively on enums defined in your own crate. Avoid a wildcard `_` arm there, so
  that adding a variant produces a compile error at every site that must handle it.
- Mark public enums and structs that may grow with `#[non_exhaustive]`.

```rust
pub enum Backend {
    C,
    Rust,
}

match backend {
    Backend::C => emit_c(workflow),
    Backend::Rust => emit_rust(workflow),
}
```

### 4.3 Common Traits

- Every public type implements `Debug`.
- Derive `Clone`, `PartialEq`, `Eq`, `Hash`, `PartialOrd`, `Ord`, and `Default` when they
  are meaningful for the type. Derive `Copy` only for small, plain-value types, and only
  when you are willing to keep it forever; removing `Copy` is a breaking change.
- Implement `Display` for types shown to users. `Debug` output is for developers and is not
  a stable format.
- Keep `Eq`, `Ord`, and `Hash` consistent with one another.

### 4.4 Integer Types and Casts

- Use `usize` for lengths and indices, `i32`/`u32`/`i64`/`u64` for values with a known
  range, and `u8` for bytes. Choose the type from the data, not from habit.
- **Avoid `as` for numeric conversions.** It truncates, wraps, and changes sign silently.
  Use `From`/`Into` for lossless conversions and `TryFrom`/`TryInto` for fallible ones.
- Arithmetic overflow panics in debug builds and wraps in release builds. On values derived
  from untrusted input, state the intent with `checked_*`, `saturating_*`, or `wrapping_*`.
- Consider `overflow-checks = true` in the release profile for code that handles untrusted
  sizes.

```rust
let count = u32::try_from(items.len())?;              // Good: fallible, explicit.
let total = price.checked_mul(quantity).ok_or(Error::Overflow)?;
let count = items.len() as u32;                       // Bad: silently truncates.
```

### 4.5 Constants and Statics

- Prefer `const` for compile-time values. Use `static` only when a single memory location
  is required.
- Avoid global mutable state. When it is unavoidable, use `OnceLock`, `LazyLock`, an atomic,
  or a `Mutex` inside a `static`.
- **Never use `static mut`.**
- Replace magic numbers with named constants, with a comment giving units or origin.

### 4.6 Strings and Paths

- Take `&str` in parameters and return `String` when ownership is transferred.
- Use `Path`/`PathBuf` for filesystem paths and `OsStr`/`OsString` for platform strings.
  Paths are not guaranteed to be UTF-8; do not round-trip them through `String`.
- Use inline format arguments: `format!("{name}: {count}")`.
- Do not index strings by byte offset unless the offset is known to be a character
  boundary; slicing in the middle of a code point panics.

---

## 5. Functions and Methods

### 5.1 Parameters and Return Values

- Borrow in parameters using the most general form: `&str` not `&String`, `&[T]` not
  `&Vec<T>`, `&Path` not `&PathBuf`, `&T` not `&Box<T>`.
- Take ownership (`String`, `Vec<T>`) only when the function stores or consumes the value.
  Do not force a clone on the caller, and do not clone internally what could be borrowed.
- Return values rather than writing through `&mut` output parameters. Return a tuple or a
  small struct for multiple results; prefer the struct once there are more than two.
- Avoid `bool` parameters; use an enum so the call site is readable. Avoid more than about
  five parameters; group them into a struct.
- Use `impl Trait` in argument position for simple generic parameters, and in return
  position to hide iterator and closure types.
- Mark functions whose result must not be ignored with `#[must_use]`. `Result` already is.

```rust
pub fn find_step<'a>(workflow: &'a Workflow, name: &str) -> Option<&'a Step> { ... }
```

### 5.2 Function Length

Prefer small, focused functions. If a function exceeds about 40 lines, consider whether it
can be broken up without harming the structure of the program.

### 5.3 Constructors

- The primary constructor is `new`. Additional constructors are named `with_*` or `from_*`.
- If `new` takes no arguments, also implement `Default`.
- Constructors that can fail return `Result<Self, Error>`; they do not panic.
- Implement conversions through `From`/`TryFrom` rather than ad hoc methods, so `?` and
  `.into()` work.

### 5.4 Control Flow

- Use early returns and `let ... else` to keep the main path unindented.
- Use `match` when handling several cases, `if let` for exactly one. Do not write
  `if x.is_some() { x.unwrap() }`.
- Omit `return` and the trailing semicolon on the final expression of a function.
- Use `?` to propagate errors; do not write the equivalent `match` by hand.
- Parentheses around `if`/`while` conditions are not written.

```rust
let Some(step) = workflow.steps().first() else {
    return Err(ParseError::NoSteps);
};
```

### 5.5 Iterators and Closures

- Prefer iterators over index loops; they remove bounds checks and off-by-one errors.
- Keep chains readable. Once a chain needs a multi-line closure with its own control flow,
  a `for` loop is usually clearer. Do not use `for_each` where a `for` loop would do.
- Indexing with `v[i]` panics when out of range. Use `.get(i)` when the index comes from
  outside the function.
- Keep closures short. A closure longer than a few lines becomes a named function.

### 5.6 Recursion

Avoid unbounded recursion; Rust does not guarantee tail-call elimination and stack overflow
aborts the process. Prefer iteration, or enforce an explicit depth limit, for data of
unbounded depth such as parsed input.

---

## 6. Ownership, Borrowing, and Lifetimes

- Every value has one clear owner. Design data structures so ownership forms a tree; reach
  for shared ownership only when the design truly requires it.
- Do not add `.clone()` to silence the borrow checker. A clone on a hot path or of a large
  value needs a reason. Cloning an `Rc`/`Arc` is written `Arc::clone(&x)` so it is visibly
  a reference-count bump.
- `Rc<RefCell<T>>` and `Arc<Mutex<T>>` are tools of last resort, not defaults. They move
  borrow errors from compile time to run time.
- Rely on lifetime elision. Write explicit lifetimes only when the compiler requires them,
  and give them meaningful names when more than one is in play (`'src`, `'ctx`).
- Do not return references to hide an allocation the caller would be better off owning.
  Use `Cow<'_, T>` when a function usually borrows but sometimes must allocate.
- Shadowing is acceptable to refine the same conceptual value (`let line = line.trim();`).
  Do not shadow a name with an unrelated value.
- Declare variables in the narrowest scope possible and initialize them at declaration.
  Use `mut` only where mutation happens.
- Resources are released by `Drop` (RAII). Do not rely on `Drop` for operations that can
  fail and must be reported, such as flushing a file; provide an explicit `close`/`finish`
  method that returns `Result`.

---

## 7. Error Handling

Rust has no exceptions. Recoverable failures are values; unrecoverable bugs are panics.

### 7.1 Result and Option

- Return `Result<T, E>` for any operation that can fail at run time: I/O, parsing, invalid
  input, missing resources.
- Return `Option<T>` when absence is a normal outcome and needs no explanation.
- Never discard a `Result`. If ignoring an error is truly correct, write `let _ = ...;`
  with a comment explaining why.
- Do not use sentinel values (`-1`, empty string) to signal failure.

### 7.2 Error Types

- **Libraries** define a concrete error type per crate or module: an enum implementing
  `std::error::Error`, typically derived with `thiserror`. Callers must be able to match
  on the failure.
- **Binaries** may use an opaque error type such as `anyhow::Error` at the top level and
  attach context while propagating.
- Error messages are lowercase, have no trailing punctuation, and describe only the current
  level; the underlying cause is exposed through `source()`, not concatenated into the
  message.
- Add context when crossing an abstraction boundary: which file, which step, which input.
- Do not use `Box<dyn Error>` or `String` as the error type of a public API.

```rust
#[derive(Debug, thiserror::Error)]
pub enum LoadError {
    #[error("failed to read workflow file")]
    Io(#[from] std::io::Error),
    #[error("line {line}: unexpected token `{token}`")]
    UnexpectedToken { line: u32, token: String },
    #[error("workflow declares no steps")]
    NoSteps,
}
```

### 7.3 Panics

- Panic only for **programmer errors**: a violated invariant that indicates a bug. Bad
  input, I/O failure, and resource exhaustion are not panics.
- Library code does not panic on any input reachable through its public API, unless the
  panic is documented under a `# Panics` heading.
- `unwrap()` is not used in production code. Use `?`, a `match`, or `expect("...")` with a
  message that states the invariant that makes failure impossible.
- `unwrap()` and `expect()` are fine in tests, examples, and doctests.
- Use `assert!` for invariants that must hold in every build, and `debug_assert!` for
  checks too expensive for release builds. Use `unreachable!("reason")` rather than a bare
  wildcard arm for states that cannot occur.
- `todo!()`, `unimplemented!()`, and `dbg!()` are never committed.
- Do not use `catch_unwind` as a general error-handling mechanism.

```rust
// Good: the message states why this cannot fail.
let first = steps.first().expect("validated workflows have at least one step");

// Bad: no information when it does fail.
let first = steps.first().unwrap();
```

### 7.4 Process Exit

`main` returns `ExitCode` or `Result`. Call `std::process::exit` only from `main`, after
cleanup, because it skips destructors.

---

## 8. Unsafe Code

Safe Rust is the default. `unsafe` is permitted only where it is necessary (FFI, a measured
performance need, or a low-level abstraction that cannot be expressed safely) and is kept
small, encapsulated, and reviewed.

- Crates that need no `unsafe` declare `#![forbid(unsafe_code)]`.
- Wrap `unsafe` in a **safe abstraction** with the smallest possible surface. Callers of the
  safe API must be unable to cause undefined behavior, whatever arguments they pass.
- Every `unsafe` block is preceded by a `// SAFETY:` comment explaining why the operation's
  requirements are met at this site. "This is safe" is not an explanation.
- Every `unsafe fn` and `unsafe trait` has a `# Safety` section in its documentation
  listing the conditions the caller must uphold.
- One unsafe operation per block where practical, so each has its own justification. Keep
  safe code out of `unsafe` blocks.
- The body of an `unsafe fn` is not an implicit unsafe block; write explicit `unsafe { }`
  blocks inside it (`unsafe_op_in_unsafe_fn`).
- Do not use `std::mem::transmute` when a safe conversion or a pointer cast exists. Do not
  use `unsafe` to bypass the borrow checker.
- Code containing `unsafe` is tested under Miri and, where applicable, sanitizers.

```rust
/// Returns the element at `index` without bounds checking.
///
/// # Safety
///
/// `index` must be less than `self.len()`.
pub unsafe fn get_unchecked(&self, index: usize) -> &T {
    // SAFETY: The caller guarantees `index < self.len()`, so the access stays
    // inside the allocation owned by `self.items`.
    unsafe { self.items.get_unchecked(index) }
}
```

---

## 9. FFI

- Confine FFI to a dedicated module or `-sys` crate. The rest of the code uses a safe Rust
  wrapper and never sees raw pointers.
- Declare foreign functions in `unsafe extern "C"` blocks, and export Rust functions with
  `#[unsafe(no_mangle)] pub extern "C" fn`.
- Types crossing the boundary are `#[repr(C)]` (or `#[repr(transparent)]`, or a fixed-width
  primitive). Use the `std::ffi` types: `c_int`, `c_char`, `CStr`, `CString`.
- Do not pass `bool`, `char`, enums with data, `String`, slices, or references across the
  boundary unless the layout on both sides has been verified.
- Document ownership for every pointer: who allocates, who frees, and with which allocator.
  Memory is freed by the side that allocated it.
- Check every pointer received from foreign code for null before dereferencing.
- A panic must not unwind into foreign code. Exported functions catch failures and convert
  them to an error code.
- Generate bindings with `bindgen`/`cbindgen` where practical instead of maintaining
  declarations by hand, and check generated bindings in or regenerate them reproducibly.

```rust
unsafe extern "C" {
    fn seq_checksum(data: *const u8, len: usize) -> u32;
}

/// Returns the runtime checksum of `data`.
pub fn checksum(data: &[u8]) -> u32 {
    // SAFETY: `data` is a valid, initialized slice of `data.len()` bytes, and
    // `seq_checksum` only reads from it for the duration of the call.
    unsafe { seq_checksum(data.as_ptr(), data.len()) }
}
```

---

## 10. Traits and Generics

- Introduce a trait when there are, or will soon be, multiple implementations or when a
  boundary must be mocked. A trait with one implementation and one caller is indirection
  without benefit.
- Prefer generics (static dispatch) for small, hot code and `dyn Trait` (dynamic dispatch)
  to reduce code size and compile time or to store heterogeneous values. Choose
  deliberately; do not make everything generic.
- Keep trait bounds minimal and put them where they are needed: on the `impl` or function,
  not on the struct definition. Move long bound lists to a `where` clause.
- Implement standard traits instead of inventing equivalents: `From` not `to_foo`
  constructors, `FromStr` for parsing, `Iterator`/`IntoIterator` for sequences, `Display`
  for user-facing text, `Default` for default values.
- Overload operators (`Add`, `Index`, ...) only when the meaning is obvious and matches the
  built-in behavior. Otherwise use a named method.
- Implement `Deref` only for smart-pointer types. Do not use it to emulate inheritance.
- Prefer composition to deep trait hierarchies. Use supertraits sparingly.
- Seal traits that are public but not meant to be implemented outside the crate.
- Avoid generic parameters that exist only for testing; avoid type-level programming that
  a reader cannot follow without the compiler's help.

---

## 11. Macros

- Prefer functions, generics, and traits over macros. Use a macro only when those cannot
  express the abstraction (variadic arguments, new syntax, compile-time code generation).
- Prefer `macro_rules!` to procedural macros. A new procedural macro needs justification:
  it adds compile time, a separate crate, and code that tools cannot see through.
- Derive macros from well-known crates (`serde`, `thiserror`, `clap`) are fine.
- Macros must be hygienic: refer to items by absolute path (`$crate::...`,
  `::std::...`), and evaluate each argument exactly once.
- A macro invocation should look like the Rust it expands to. Do not use macros to define
  a private dialect of the language.
- Document every exported macro with an example.

---

## 12. Concurrency and Async

- Rely on `Send` and `Sync` to enforce thread safety. Never write `unsafe impl Send` or
  `unsafe impl Sync` without a `// SAFETY:` comment proving the claim.
- Prefer message passing (channels) or scoped threads (`std::thread::scope`) over shared
  mutable state. When state is shared, it sits behind `Mutex`, `RwLock`, or an atomic.
- Keep critical sections short. Do not call unknown code (callbacks, trait objects) while
  holding a lock. Document lock ordering wherever two locks can be held together.
- Decide and document how each lock handles poisoning; do not scatter
  `.lock().unwrap()` without having made that decision.
- Atomics: state the reason for each `Ordering`. Use `SeqCst` when in doubt; weaker
  orderings need a comment.
- **Async** is used only where it earns its complexity, typically for many concurrent I/O
  operations. CPU-bound and simple sequential programs stay synchronous.
  - Use one async runtime per program.
  - Never hold a `std::sync::Mutex` guard, or any other blocking lock, across an `.await`.
  - Do not block the executor: move blocking I/O and long computation to a blocking pool.
  - Make cancellation safe: assume any `.await` can be the last line that runs.
- Spawned threads and tasks are joined or their handles deliberately dropped with a comment;
  do not leak them silently.

---

## 13. Naming

Names should be descriptive; avoid abbreviations unfamiliar outside your project. Longer scope
warrants a longer, more descriptive name. Rust naming follows RFC 430 and is checked by the
compiler; do not silence the `nonstandard_style` lints.

| Entity                          | Convention                    | Example                        |
|---------------------------------|-------------------------------|--------------------------------|
| Crates                          | `snake_case`, single word preferred | `seq_runtime`            |
| Modules and files               | `snake_case`                  | `build_cache.rs`               |
| Types (struct/enum/union/alias) | `UpperCamelCase`              | `Workflow`, `BuildManifest`    |
| Traits                          | `UpperCamelCase`              | `Backend`, `ModelAdapter`      |
| Enum variants                   | `UpperCamelCase`              | `Backend::Rust`                |
| Functions and methods           | `snake_case`                  | `parse_workflow()`             |
| Local variables and parameters  | `snake_case`                  | `step_count`, `source_path`    |
| Struct fields                   | `snake_case`                  | `capacity`                     |
| Constants and statics **[differs]** | `SCREAMING_SNAKE_CASE`    | `MAX_STEPS`                    |
| Type parameters                 | Short `UpperCamelCase`        | `T`, `K`, `V`, `Item`          |
| Lifetimes                       | Short lowercase               | `'a`, `'src`                   |
| Macros                          | `snake_case!`                 | `seq_check!`                   |
| Cargo features                  | Short lowercase, no prefix    | `serde`, `png`                 |

**Notes**

- **[differs]** The C and C++ guides name constants `kPascalCase`. Rust uses
  `SCREAMING_SNAKE_CASE`; the compiler warns on anything else.
- Acronyms count as one word: `HttpClient`, `PngEncoder`, `Uuid`, not `HTTPClient`.
- Do not repeat the module or crate name in an item: `parser::Error`, not
  `parser::ParserError`; `seq_runtime::Context`, not `seq_runtime::SeqRuntimeContext`.
- Getters are named for the field, without a `get_` prefix: `fn name(&self) -> &str`.
  Predicates start with `is_` or `has_`.
- Conversion methods follow the standard prefixes:

  | Prefix   | Cost      | Ownership                         | Example             |
  |----------|-----------|-----------------------------------|---------------------|
  | `as_`    | Free      | Borrowed to borrowed              | `as_bytes()`        |
  | `to_`    | Expensive | Borrowed to owned                 | `to_string()`       |
  | `into_`  | Variable  | Owned to owned, consumes `self`   | `into_bytes()`      |

- Iterator methods are `iter()`, `iter_mut()`, and `into_iter()`.
- Use the standard library's vocabulary: `push`, `len`, `is_empty`, `get`, `insert`,
  `contains`. Do not invent synonyms.
- Prefix an intentionally unused binding with `_`; do not leave unused-variable warnings.
- Do not name a crate with a `-rs` or `rust` affix.

---

## 14. Comments and Documentation

Comments are essential, but the best code is self-documenting. Use `//` line comments;
do not use `/* */` block comments.

### 14.1 Doc Comments

- Every public item (crate, module, type, trait, function, method, constant, macro) has a
  `///` doc comment. Enable `#![warn(missing_docs)]` in library crates.
- Every crate root and every non-trivial module starts with a `//!` comment describing its
  purpose and how its parts fit together.
- The first line is a single-sentence summary in the third person: "Returns the ...",
  "Parses a ...". It appears alone in indexes and search results.
- Use these headings, in this order, when they apply:
  - `# Errors` — the conditions under which a `Result`-returning function returns `Err`.
  - `# Panics` — every condition under which the function can panic.
  - `# Safety` — required for every `unsafe fn` and `unsafe trait`.
  - `# Examples` — a compiling, tested example for anything non-obvious.
- Link to other items with intra-doc links: ``[`Workflow`]``, ``[`parse`]``.
- Document what the item does and how to use it, not how it is implemented. Do not restate
  the signature in prose.

````rust
/// Parses a Seq workflow from `source`.
///
/// Steps are returned in source order.
///
/// # Errors
///
/// Returns [`LoadError::UnexpectedToken`] if `source` is not valid Seq, and
/// [`LoadError::NoSteps`] if it declares no steps.
///
/// # Examples
///
/// ```
/// let workflow = seq::parse(include_str!("../examples/hello/src/main.seq"))?;
/// assert_eq!(workflow.steps().len(), 1);
/// # Ok::<(), seq::LoadError>(())
/// ```
pub fn parse(source: &str) -> Result<Workflow, LoadError> {
    ...
}
````

### 14.2 File Comments

Begin each file with license boilerplate if the project requires it, followed by the `//!`
module documentation.

### 14.3 Implementation Comments

Comment tricky, non-obvious, or important code. Explain *why*, not *what*. Do not restate
code. Invariants that the type system cannot express are written down where they are relied
upon.

### 14.4 SAFETY Comments

Every `unsafe` block carries a `// SAFETY:` comment (see [Unsafe Code](#8-unsafe-code)).
These are mandatory and are enforced by Clippy.

### 14.5 Lint Suppressions

Suppress a lint at the narrowest scope, prefer `#[expect(...)]` to `#[allow(...)]` so the
attribute is flagged once it is no longer needed, and always give a reason:

```rust
#[expect(clippy::too_many_arguments, reason = "mirrors the C runtime entry point")]
```

### 14.6 TODO Comments

Use `TODO` with a bug/issue reference or owner, and a description:

```rust
// TODO(bug 12345): Replace linear scan with a hash map once inputs exceed 10k.
```

### 14.7 Punctuation and Grammar

Write comments as complete sentences with proper capitalization and punctuation. Short
end-of-line comments may be fragments.

---

## 15. Formatting

All code is formatted by `rustfmt` with the default style (see [Tooling](#17-tooling)).
Formatting is not debated in review and is checked in CI. The rules below describe the
result; where `rustfmt` and this section disagree, `rustfmt` wins.

### 15.1 Line Length **[differs]**

Maximum **100 characters**, the `rustfmt` default (the C and C++ guides use 80).
Exceptions: long URLs in comments and string literals that cannot be sensibly split.
`rustfmt` does not wrap comments; keep them within the limit by hand.

### 15.2 Indentation **[differs]**

- **4 spaces** per level, the `rustfmt` default (the C and C++ guides use 2). No tabs.
- Continuation lines use block indentation (one extra level), not visual alignment with an
  opening delimiter.

### 15.3 Braces

Opening brace on the same line as the item or statement; closing brace on its own line.
Braces are always required around `if`, `else`, `for`, `while`, and `loop` bodies.

```rust
fn max(a: i32, b: i32) -> i32 {
    if a > b { a } else { b }
}
```

### 15.4 Function Signatures and Calls

Keep everything on one line when it fits. Otherwise put each parameter on its own line
with a trailing comma.

```rust
pub fn encode_frame(encoder: &Encoder, frame: &Frame, out: &mut Vec<u8>) -> Result<(), Error> {
    ...
}

pub fn encode_frame_with_a_really_long_name(
    encoder: &Encoder,
    frame: &Frame,
    out: &mut Vec<u8>,
) -> Result<(), EncodeError> {
    ...
}
```

### 15.5 Trailing Commas

Every multi-line comma-separated list (parameters, arguments, struct fields, enum variants,
match arms with blocks) ends with a trailing comma. Single-line lists do not.

### 15.6 Where Clauses

Put each bound on its own line, with the opening brace on a line by itself.

```rust
fn merge<K, V>(left: &HashMap<K, V>, right: &HashMap<K, V>) -> HashMap<K, V>
where
    K: Eq + Hash + Clone,
    V: Clone,
{
    ...
}
```

### 15.7 Method Chains

When a chain does not fit on one line, break before each `.` and indent one level.

```rust
let totals: Vec<u64> = transactions
    .iter()
    .filter(|t| t.is_settled())
    .map(|t| t.quantity * t.price)
    .collect();
```

### 15.8 Match Expressions

Each arm is on its own line. Use a block when the body does not fit on the arm's line; a
block arm has no trailing comma.

```rust
match status {
    Status::Ok => report_success(),
    Status::Failed { code } => {
        log_failure(code);
        return Err(Error::StepFailed(code));
    }
}
```

### 15.9 Attributes

Each attribute is on its own line above the item. Combine derives into a single
`#[derive(...)]`. Place doc comments above attributes.

### 15.10 Horizontal and Vertical Whitespace

- Spaces around binary operators and after `:` and `,`; none before them.
- No trailing whitespace.
- One blank line between items; never more than one consecutive blank line. Don't start or
  end a block with a blank line.
- Files end with a single newline and use `\n` line endings.

---

## 16. Testing

- Every public function has tests. Bug fixes come with a regression test.
- **Unit tests** live in a `#[cfg(test)] mod tests` at the bottom of the file they test and
  may exercise private items.
- **Integration tests** live in `tests/` and use only the public API.
- **Doctests** keep the examples in documentation compiling and correct.
- Test names describe the behavior under test: `parse_rejects_workflow_without_steps`, not
  `test_parse_2`. Do not prefix names with `test_`.
- One behavior per test. Prefer `assert_eq!`/`assert_ne!` to `assert!(a == b)` for useful
  failure output, and `assert!(matches!(...))` for enum shapes.
- Tests may return `Result` and use `?`. `unwrap()` and `expect()` are acceptable in tests.
- Tests are deterministic and independent: no reliance on execution order, wall-clock time,
  network access, or shared global state. Use a temporary directory for filesystem work.
- Run code containing `unsafe` under **Miri**. Run sanitizers (ASan, TSan) on FFI and
  concurrent code in CI.
- Fuzz any function that parses untrusted input (`cargo fuzz`), and use property-based
  tests (`proptest`) where an invariant is easier to state than examples are to enumerate.

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_rejects_workflow_without_steps() {
        let err = parse("name = \"demo\"\n").unwrap_err();
        assert!(matches!(err, LoadError::NoSteps));
    }
}
```

---

## 17. Tooling

### 17.1 Recommended `rustfmt.toml`

Use the default style. The file pins it and adds two stable shorthand options; do not add
unstable options, which require nightly.

```toml
edition = "2024"
style_edition = "2024"
max_width = 100
newline_style = "Unix"
use_field_init_shorthand = true
use_try_shorthand = true
```

### 17.2 Lints

Configure lints once in the workspace `Cargo.toml` and inherit them in every member with
`[lints] workspace = true`.

```toml
[workspace.lints.rust]
unsafe_op_in_unsafe_fn = "deny"
missing_docs = "warn"
missing_debug_implementations = "warn"
unreachable_pub = "warn"

[workspace.lints.clippy]
all = { level = "warn", priority = -1 }
pedantic = { level = "warn", priority = -1 }
undocumented_unsafe_blocks = "deny"
multiple_unsafe_ops_per_block = "warn"
allow_attributes_without_reason = "warn"
unwrap_used = "warn"
dbg_macro = "deny"
todo = "deny"
```

`clippy::pedantic` is noisy by design; allow individual lints per project with a recorded
reason rather than dropping the group. Permit `unwrap` in tests through `clippy.toml`:

```toml
allow-unwrap-in-tests = true
allow-expect-in-tests = true
```

### 17.3 CI Checks

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --all-features --locked -- -D warnings
cargo test --workspace --all-features --locked
RUSTDOCFLAGS="-D warnings" cargo doc --workspace --no-deps --locked
cargo deny check
```

### 17.4 Release Profile

State optimization settings explicitly rather than relying on defaults that readers must
look up.

```toml
[profile.release]
opt-level = 3
overflow-checks = true
debug = "line-tables-only"
```

---

## 18. Exceptions to the Rules

- **Existing non-conforming code:** follow the local conventions of the file you are editing
  rather than mixing styles. Reformat only in dedicated, behavior-neutral changes.
- **Third-party code:** leave vendored code in its original style and exclude it from
  `rustfmt` and Clippy.
- **Generated code:** bindings and other generated sources are exempt from formatting and
  lint rules, but must be clearly marked as generated and kept in separate files.
- **FFI names:** items that mirror a foreign API may keep the foreign naming inside the thin
  binding layer, with a scoped `#[expect(nonstandard_style, reason = "...")]`.
- **Formatting:** use `#[rustfmt::skip]` only on a specific item where manual layout
  (a lookup table, a matrix) is materially more readable, never on a whole module.

When in doubt, **be consistent**. If code you add looks drastically different from the code
around it, the discontinuity distracts readers from what matters.

---

## 19. Quick Reference

| Topic              | Rule                                                        |
|--------------------|-------------------------------------------------------------|
| Edition            | Rust 2024, stable toolchain, pinned in `rust-toolchain.toml` |
| Indentation        | 4 spaces, no tabs                                           |
| Line length        | 100 columns                                                 |
| Formatting tool    | `rustfmt` defaults; checked in CI                           |
| Linting            | Clippy `all` + `pedantic`; `-D warnings` in CI, not in source |
| Types and traits   | `UpperCamelCase`                                            |
| Functions, variables, modules | `snake_case`                                     |
| Constants          | `SCREAMING_SNAKE_CASE`                                      |
| Imports            | std / external / crate groups; no globs                     |
| Visibility         | Private by default; widen only as needed                    |
| Parameters         | Borrow the general form: `&str`, `&[T]`, `&Path`            |
| Conversions        | `From`/`TryFrom`; avoid `as` for numeric casts              |
| Errors             | `Result` everywhere; typed errors in libraries; propagate with `?` |
| Panics             | Bugs only; no `unwrap()` in production; `expect` states the invariant |
| Unsafe             | Minimal, encapsulated, `// SAFETY:` on every block, `# Safety` on every `unsafe fn` |
| Globals            | Avoid; `OnceLock`/`LazyLock`/atomics; never `static mut`    |
| Docs               | `///` on every public item; `# Errors`, `# Panics`, `# Safety`, `# Examples` |
| Tests              | Unit tests in-file, integration tests in `tests/`, doctests, Miri for `unsafe` |
| Dependencies       | Reviewed, locked, minimal features, checked by `cargo deny` |
| Forbidden          | `static mut`, `todo!`/`dbg!` in commits, glob imports, nightly features, `#![deny(warnings)]` |

---

*Based on the Rust Style Guide (https://doc.rust-lang.org/style-guide/) as enforced by
`rustfmt`, the Rust API Guidelines (https://rust-lang.github.io/api-guidelines/), and Clippy,
with general principles borrowed from the Google C++ Style Guide
(https://google.github.io/styleguide/cppguide.html) and Google's Comprehensive Rust
(https://google.github.io/comprehensive-rust/).*
