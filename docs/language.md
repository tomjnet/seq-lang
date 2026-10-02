# The Seq language

**Grammar version:** 1 (seqc 0.1)

A Seq source file declares which model writes the program, which language the program is written in, the project name, and an ordered list of steps. Each step holds one or more requests in natural language. `seqc check <file.seq>` validates a file against everything on this page without a model or a compiler.

```seq
model("https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct")
backend.C()
name = "top3Company"

step step1():
    ask("create a database file company.txt with 5 sample stock transactions")

step step2():
    ask("for each transaction created give me the total revenue of the company")

step step3():
    ask("create an image chart showing the 3 companies with the highest revenue")
```

## Header declarations

The three header declarations are each required exactly once. They may come in any order, and all of them come before the first step.

| Declaration | Rule |
| --- | --- |
| `model("<url>")` | A call with exactly one nonempty string. The URL has the form `https://huggingface.co/<owner>/<repo>`, optionally followed by `/tree/<revision>` to pin a revision in source. |
| `backend.C()` | Selects the generated language. `C` is the only backend in v0.1 and takes no arguments. |
| `name = "<name>"` | The project name and the basename of generated artifacts. |

`model` and `backend` are calls and `name` is an assignment. Writing `model = "..."` or `backend = "c"` is a syntax error whose hint shows the call form.

Two forms are reserved for later versions. The parser recognizes them and reports a clear error:

- `model("<url>", "<server>")`: a second argument that names an inference server. Rejected with "model server argument is not supported in v0.1" at the second argument.
- `backend.Rust()`: rejected with "backend.Rust() is not supported in v0.1".

The name is used in file names, so it is restricted: ASCII letters, digits, underscores, and hyphens, starting with a letter or underscore, at most 64 characters.

## Steps and requests

```seq
step <name>():
    ask("<request>")
    ask("<another request>")
```

- A workflow has at least one step. Steps run in source order.
- A step name is an identifier (`[A-Za-z_][A-Za-z0-9_]*`). It is a label, not a dependency or something that can be called. Names are unique. The reserved words `model`, `backend`, `name`, `step`, and `ask` cannot be step names.
- The parameter list is always empty.
- A step body holds one or more `ask("...")` statements, one per line, indented exactly four spaces. Requests keep their order.
- `ask` takes parentheses and exactly one nonempty string literal.

`ask()` is a compile-time directive. The model receives the whole workflow at once, so a later step can use what an earlier step creates. The generated executable never contacts the model.

Seq has no expressions, branches, loops, imports, or parallel steps. The generated program may use them internally.

## Lexical rules

| Topic | Rule |
| --- | --- |
| Encoding | UTF-8. One leading byte-order mark is skipped. Invalid UTF-8, NUL bytes, and control characters are errors. |
| Line endings | LF or CRLF. CRLF is normalized to LF before lexing, so both give the same result and the same build cache key. A lone CR is an error. |
| Indentation | Zero spaces for declarations and step headers, four spaces for `ask()`. Anything else is an error. Tabs are rejected everywhere. |
| Blank lines, comments | Blank lines and lines that hold only a comment carry no indentation meaning. A comment runs from `#` to the end of the line, except inside a string. |
| Trailing spaces, final newline | Trailing spaces are ignored. A missing final newline is accepted. |
| Strings | Double-quoted and single-line. Escapes: `\"`, `\\`, `\n`, `\t`. Any other escape is an error. |

## Limits

Exceeding a limit is a validation error. Nothing is truncated.

| Limit | Value |
| --- | --- |
| Source file size | 1 MiB |
| `name` length | 64 characters |
| One request, decoded | 4096 bytes |
| Steps per workflow | 64 |
| `ask()` statements per step | 16 |

## Diagnostics

Every problem is reported as

```text
path:line:column: error[E0204]: ask requires parentheses
        ask "hello"
            ^
    hint: write ask("...")
```

Lines and columns start at 1. Columns count Unicode code points. Diagnostics go to standard error, sorted by position. Color is used only when standard error is a terminal, and never when `NO_COLOR` is set. `seqc` reports every error it can find in one run, and all of them before any model access.

The codes are stable.

### Lexical (E01xx)

| Code | Meaning |
| --- | --- |
| E0101 | The file is not valid UTF-8. |
| E0102 | NUL byte or control character. |
| E0103 | Tab character. |
| E0104 | Carriage return without a line feed. |
| E0105 | Unterminated string literal. |
| E0106 | Unknown escape sequence. |
| E0107 | Unexpected character. |
| E0108 | Indentation is not 0 or 4 spaces. |
| E0109 | The file is larger than 1 MiB. |
| E0110 | The file cannot be read. |

### Syntax (E02xx)

| Code | Meaning |
| --- | --- |
| E0201 | Unexpected token or unknown declaration. The message says what was expected. |
| E0202 | `model` written as an assignment. |
| E0203 | `backend` written as an assignment. |
| E0204 | `ask` without parentheses. |
| E0205 | Header declaration after the first step. |
| E0206 | `ask()` outside a step. |
| E0207 | Indented line that does not belong to a step. |
| E0208 | Reserved word used as a step name. |
| E0209 | Step with no `ask()` statements. |
| E0211 | Step with parameters. |
| E0212 | Statement other than `ask()` inside a step. |
| E0213 | `ask()` with no argument, more than one, or one that is not a string. |

### Validation (E03xx)

| Code | Meaning |
| --- | --- |
| E0301 | Missing `model` declaration. |
| E0302 | Missing `backend` declaration. |
| E0303 | Missing `name` declaration. |
| E0304 | Duplicate header declaration. |
| E0305 | `backend.Rust()` is reserved and not supported in v0.1. |
| E0306 | Unknown backend. |
| E0307 | `model()` with no argument or more than two. |
| E0308 | `model()` argument that is not a string. |
| E0309 | Empty `model()` argument. |
| E0310 | The reserved model server argument, not supported in v0.1. |
| E0311 | The model is not a `https://huggingface.co/<owner>/<repo>` URL. |
| E0312 | Invalid project name. |
| E0313 | Project name longer than 64 characters. |
| E0314 | Workflow with no steps. |
| E0315 | Duplicate step name. |
| E0316 | Empty request. |
| E0317 | Request longer than 4096 bytes. |
| E0318 | More than 64 steps. |
| E0319 | More than 16 `ask()` statements in a step. |
| E0320 | `backend.C()` with arguments. |

An empty file is reported as its three missing header declarations at line 1, column 1.

## Exit statuses of seqc

| Status | Meaning |
| --- | --- |
| 0 | Success. |
| 1 | An unexpected failure inside seqc itself. |
| 2 | Bad command line or setting. |
| 3 | The source file is not a valid workflow. |
| 4 | The model could not be resolved or reached, or its response was unusable. |
| 5 | The generated program or its tests were rejected, or did not compile within the repair limit. |
| 6 | The generated program failed, hit a limit, or produced invalid outputs. |
| 7 | A filesystem or security rule stopped the run: bad project layout, rejected input, output collision, busy project, or unavailable isolation. |
| 8 | A prerequisite is missing: GCC, g++, the static C library, Google Test, the seqc runtime, or curl. |
| 130 | Cancelled with Ctrl-C. |

The generated executable itself exits with 0 when every step succeeds and with 70 when a step fails.
