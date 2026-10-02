# Changelog

Versions follow [semantic versioning](https://semver.org/).

## 0.1.0 — unreleased

First implementation of the plan in seq-lang. Linux x86_64.

### Added

- The Seq language, grammar version 1: `model("...")`, `backend.C()`, `name = "..."`, and ordered `step` blocks of `ask("...")` requests, with source-located diagnostics and stable error codes.
- `seqc new`, `seqc check`, `seqc doctor`, `seqc model pull`, `seqc clean`, and `seqc <file.seq>` with `--build-only`, `--rebuild`, `--force`, and `--quiet`.
- Whole-workflow planning and C synthesis through a model adapter, with schema-constrained responses, a context budget check, and bounded repair of hygiene, compile, and link errors.
- A C backend that emits `<name>.c`, optimized assembly `<name>.s`, and a static executable linked from that assembly.
- Generated Google Test tests for every accepted build, built with bounded repair and run isolated.
- A runtime library with a step driver, path helpers, error reporting, and a bar chart helper that writes PNG.
- A build cache keyed on the source, inputs, locked model, settings, runtime, and toolchain.
- A sandbox for compilers and generated code built on Landlock and seccomp, with resource limits.
- Staged output publication with a rerun rule that never replaces a file `seqc` did not create.
- A model lock file, `seq.lock`, and pinned, hash-verified model download.
- `install.sh`, which installs a checksum-verified release package or builds from source, without root.
- A release workflow that builds, tests, packages, and publishes `seqc-linux-x86_64.tar.gz` for a `vX.Y.Z` tag.
- A website under `website/`, published to GitHub Pages.

### Known limitations

- **Never run with a real model.** The model feasibility spike and all live-model pass rates are open. See `docs/decisions.md`.
- The sandbox is proven only on WSL2 so far.
- Linux x86_64 only. No Rust backend, no remote inference, no model server argument.
- Kind checks on outputs are structural only.
