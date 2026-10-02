# Decisions and open items

Leaves choices open until stated gates. This page records what was chosen when v0.1 was implemented, what was measured, and what is still unverified. It is the place to look before trusting a claim about a real model.

## Not done: model feasibility (Phase 0)

The plan puts a model feasibility spike before any compiler work. **It has not been run.** The machine this was built on had no llama.cpp and no model weights, and neither was downloaded.

Consequences:

- No measurement exists of how often the reference model produces a valid plan, compiling C, correct behavior, or compiling tests. The plan's pass-rate thresholds (9, 8, and 7 of 10; 6 of 10 for the chart example) are untested.
- The choice of the Instruct variant, the one-function-per-step program shape, and the result policy for generated tests are the plan's proposals, carried over unconfirmed.
- The llama.cpp adapter has only been run against `tests/integration/mock_llama_server.py`. The request shape follows llama.cpp's documented OpenAI-compatible endpoint (`/v1/chat/completions` with `response_format` of type `json_schema`, `/tokenize`, `/health`, `/props`), but it has not met a real `llama-server`.
- The pinned revision and SHA-256 of the reference GGUF file were read from the Hugging Face API on 2026-09-30 and not confirmed by a download. `seqc model pull` verifies the hash, so a wrong pin fails closed.

To run the spike: install llama.cpp, run `seqc model pull` in a scaffolded project, then build the `examples/` projects at least ten times each with `--rebuild` and record the outcomes.

## Decided

### Before Phase 0

| Item | Decision |
| --- | --- |
| Project naming | Kept: language Seq, command `seqc`, extension `.seq`, runtime prefix `seq_`. The overlap with the archived `seq-lang/seq` project still needs a deliberate decision before anything is published. |

### Phase 1 gate

| Item | Decision |
| --- | --- |
| Inference runtime | llama.cpp `llama-server`, started by `seqc` per build on a loopback port and reached over plain HTTP. An already running loopback server can be selected with `model.endpoint`. No in-process linking. The llama.cpp version is not pinned yet: there was none to test against. |
| Model acquisition | `curl`, started by `seqc model pull` with an argument array. One pinned file, verified by SHA-256. |
| Reference toolchain | Ubuntu 24.04 LTS: GCC 13.3, CMake 3.28, Ninja 1.11. `seqc` itself needs C++20. |
| Isolation | Landlock plus a seccomp filter, `no_new_privs`, and resource limits, applied between fork and exec. Minimum Landlock ABI 3 (kernel 6.2). See [security.md](security.md). |
| Supported environments | Proven on WSL2 (kernel 6.18), both on the Linux filesystem and on a Windows drive (9p), which needed the supervisor to hold the rule descriptors open; see [security.md](security.md). Ubuntu 24.04 on a stock kernel and the CI runner are not yet proven; the gate is open until they are. |
| Linkage | Static. `seqc doctor` checks that a static program links. |
| Language limits | As proposed: name 64 characters, request 4096 bytes, 64 steps, 16 requests per step. Added: source file 1 MiB. |
| Resource limits | See the table in [security.md](security.md). All are settings. |
| Exit statuses | 0 to 8 and 130, listed in [language.md](language.md). |
| Diagnostic codes | E01xx lexical, E02xx syntax, E03xx validation, listed in [language.md](language.md). |
| Dependencies of `seqc` | None beyond the C++ standard library. JSON, the TOML subset, SHA-256, and the loopback HTTP client are in-house. |
| Unit-test framework for `seqc` | An in-house harness (`tests/unit/test_harness.hpp`), so the suite builds wherever `seqc` builds. |
| PNG encoder and font | In-house, MIT like the rest: a fixed-Huffman deflate encoder with run-length matches, and a 5 by 7 bitmap font drawn for this project. |
| Runtime header | `runtime/include/seq_runtime.h`, version 1. See [runtime.md](runtime.md). |
| Google Test | The distribution package (`libgtest-dev`, 1.14.0 on Ubuntu 24.04), or any prefix named by `toolchain.gtest_root`. The version is recorded through the compiler version and prefix in the build manifest, not pinned by `seqc`. Verified with 1.14.0: generated tests link statically, run inside the sandbox, and their results are read correctly (`integration.system_gtest`). The static link prints a harmless `getaddrinfo` warning from Google Test's unused streaming listener. |
| `output/temp/Makefile` | Stays an unchanged copy of the template. The runtime paths are passed on the `make` command line; [runtime.md](runtime.md) shows how. |

### Phase 3 and 4 gates

| Item | Decision |
| --- | --- |
| Structured generation | JSON schema per request, enforced by the inference server. Plan as one object; source as one string member. |
| Hygiene findings | Sent back for repair within the same attempt budget as compile errors. |
| Unsupported dependency in a plan | Fails the build without repair. |
| Failing generated test cases | Reported and recorded; they do not block execution. A test file that does not compile fails the build. Unconfirmed until Phase 0 data exists. |
| Link order | `-lgtest_main -lgtest`, the reverse of the plan's representative command, because static archives resolve left to right. |

### Phase 5 gate

| Item | Decision |
| --- | --- |
| Diagnostic retention | Run records are kept until `seqc clean`. No automatic expiry. |
| Published-file record | `output/temp/published.json` accumulates every file `seqc` published with its hash. `seqc clean --all` keeps it, so reruns keep working after a clean. |

### Phase 6 gate

| Item | Decision |
| --- | --- |
| Example data | `examples/top3Company` states the exact `company.txt` format and the five transactions in its prompts, so an independent oracle has fixed data to check. |
| "Revenue" | The example asks for a per-company *sales total*, the sum of quantity times price over its transactions. That is transaction value, not accounting revenue. |
| Pass rate and fallback | Open: needs a real model. |

## Differences from the plan

- `seqc --settings` and `--set section.key=value` were added so that the settings chain has a flag level.
- `output/temp/build.json` and `output/temp/published.json` are compiler state kept beside the artifacts, in addition to the per-run manifests the plan lists.
- A source file size limit of 1 MiB was added.
- The build sandbox grants `/etc/ld.so.cache` and `/etc/alternatives` rather than nothing from `/etc`, because the dynamically linked compiler needs them.
- The CI workflow exists but has not run.

## Open

1. Run Phase 0 and record the report. Revise the model, the program shape, or the thresholds from its data.
2. Prove the sandbox on Ubuntu 24.04 with its stock kernel and on the CI runner.
3. Pin and test a llama.cpp version; confirm constrained decoding and token counting against it.
4. Decide the project naming question before publishing.
5. Publish a release tarball (`cpack` writes it with a SHA-256 checksum).
6. After v0.1: the reserved `model("<url>", "<server>")` form and the Rust backend.
