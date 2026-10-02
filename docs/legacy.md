# Seq and Seq Legacy

There are two Seq projects. The seq-lang repository holds `seqc`. [seq-lang-legacy](https://github.com/tomjnet/seq-lang-legacy) holds `seqc_legacy`, a proof of concept that compiles a fixed subset of Seq with no language model and no external toolchain. Both read `.seq` files with the same `step` and `ask()` shape, and both scaffold a project with `new`, but they answer a request in opposite ways: `seqc` asks a model to write a program for it, and `seqc_legacy` recognizes it as one of two built-in requests and generates the machine code itself.

The same example in each language:

```seq
# seq-lang
model("https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct")
backend.C()
name = "top3Company"

step step1():
    ask("create a database file company.txt with 5 sample stock transactions")
```

```seq
# seq-lang-legacy
name = "top3Company"

step step1():
    ask("create file company.txt with 5 sample-company-transaction-db")
```

## seq-lang and seq-lang-legacy

| | seq-lang | seq-lang-legacy |
| --- | --- | --- |
| Purpose | The Seq language: workflows of natural-language requests that a model turns into a program | Proof of concept of a Seq compiler that depends on nothing |
| Compiler | `seqc` | `seqc_legacy` |
| Header declarations | `model("<url>")`, `backend.C()`, and `name = "<name>"`, each required | `name = "<name>"` only |
| What an `ask()` may say | Any request in natural language, up to 4096 bytes | One of two fixed requests: `create file <file> with <N> <dataset>` and `for-each transaction get <N> top <filter>` |
| Data a workflow can use | Files in `input/`, and whatever the generated program creates | One built-in dataset of 500 rows, `sample-company-transaction-db` |
| Operations | Whatever the model can write in C against the runtime API | One built-in filter, `total-revenue-by-company` |
| Results | Text files and PNG images, published to `output/` | A text file in `output/` and lines on the console |
| Same source, same program | No. The model writes the program, so two builds can differ | Yes. Every stage is deterministic |
| Diagnostics | `path:line:column: error[E....]` with a hint; codes `E01xx` to `E03xx` | The same format; adds `E04xx` codes for its built-in requests |
| Examples | `hello`, `top3Company` | `top3Company` |
| Website | Astro site under `website/` | Static HTML pages under `website/` |
| Continuous integration | Build and test, release, and website workflows | Website workflow only |
| Status | 0.1.0, not yet verified with a real model | 0.1.0, complete for its two requests |

## seqc and seqc_legacy

| | `seqc` | `seqc_legacy` |
| --- | --- | --- |
| Who writes the program | A language model, at compile time | The compiler's own code generator |
| Language model | Required: llama.cpp `llama-server` and model weights of about 1.1 GB | None |
| External toolchain | GCC with the static C library, `g++`, Google Test | None: its own assembler and linker |
| Stages | Parse, validate, plan, generate C, policy check, `gcc -O3 -S`, link, generated tests, run, publish | Lexer, parser, semantic analysis, IR, code generator, assembler, linker, run |
| Where the work happens | At run time, in the generated program | At compile time: the filter is evaluated in the IR stage and the program only writes bytes the compiler computed |
| Generated program | A C program, one function per step, linked statically with the Seq runtime library | `_start` and one function per step that call the kernel directly; linked against nothing |
| Artifacts in `output/temp/` | `.c`, `.s`, `.bin`, `test/`, `build.json`, `runs/<run-id>/` | `.tokens`, `.ast`, `.ir`, `.asm`, `.lst`, `.bin` |
| Repair loop | At most 2 repairs when the generated code is rejected | None; there is nothing to repair |
| Generated tests | Google Test cases written by the model, built and run in the sandbox | None |
| Build cache | Reuses the accepted build while source, inputs, model, and toolchain are unchanged | None; every run rebuilds |
| Running the program | In a sandbox: Landlock, seccomp, `no_new_privs`, resource limits | Executed directly, with no isolation |
| Target | Linux x86_64, kernel 6.2 or newer | Linux x86_64 |
| On other systems | Can scaffold, check, and clean projects | Can also build the ELF file with `--build-only`, but not run it |
| Commands | `new`, `<file.seq>`, `check`, `doctor`, `model pull`, `clean`, `--settings` | `new`, `<file.seq>`, `check` |
| Options for `<file.seq>` | `--build-only`, `--rebuild`, `--force`, `--quiet` | `--build-only` |
| Settings | `--set`, environment variables, `config.toml` | None |
| Exit statuses | 0 to 8 and 130 | 0, 1, 2, 3, 6, 7, with the same meanings as in `seqc` |
| Implementation language | C++20 | C++11 |
| To build it | CMake 3.28, Ninja, GCC 13 or newer | CMake 3.20, Ninja, any C++11 compiler |
| Install | `install.sh`: a release package or a source build into `~/.local`, no root | `install-seqc.sh`: copies the built binary to `/usr/local/bin` with `sudo` |

Use `seqc_legacy` to read a whole compiler from lexer to ELF file, or to get the same executable from the same source every time. Use `seqc` for requests outside those two forms.
