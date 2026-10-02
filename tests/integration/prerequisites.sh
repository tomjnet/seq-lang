#!/bin/bash
# Missing prerequisites are reported as such, with their own exit status,
# before any inference, and never as generated-code errors.
. "$SEQ_INTEGRATION/lib.sh"

project hello hello
script hello --plan "$W/hello/plan.json" --generate "$W/hello/steps.c" \
  --test "$W/hello/tests.cc"

# --- Missing C compiler. ------------------------------------------------------
seqc --set toolchain.cc=/nonexistent/bin/gcc src/main.seq
expect_status 8
expect_err "C compiler '/nonexistent/bin/gcc' was not found"
expect_model_calls ""

# --- Missing C++ compiler. ----------------------------------------------------
seqc --set toolchain.cxx=no-such-g++ src/main.seq
expect_status 8
expect_err "C++ compiler 'no-such-g++' was not found"
expect_err "it builds the generated tests"
expect_model_calls ""

# --- Missing Google Test. -----------------------------------------------------
# Only checkable where Google Test is not installed system-wide.
if [ ! -e /usr/include/gtest/gtest.h ]; then
  seqc --set toolchain.gtest_root= src/main.seq
  expect_status 8
  expect_err "Google Test is not usable"
  expect_err "libgtest-dev"
  expect_model_calls ""
fi
seqc --set toolchain.gtest_root=/nonexistent/gtest src/main.seq
expect_status 8
expect_err "toolchain.gtest_root is not a directory"

# --- Missing runtime or templates. --------------------------------------------
mkdir -p "$SEQ_WORK/empty-home"
seqc --set paths.home="$SEQ_WORK/empty-home" src/main.seq
expect_status 8
expect_err "seqc template not found"
SEQC_HOME="$SEQ_WORK/empty-home" "$SEQC" new viaenv >"$OUT" 2>"$ERR"
STATUS=$?
expect_status 7
expect_err "seqc template not found"
expect_no_file viaenv

# --- Wrong entry point. -------------------------------------------------------
cp src/main.seq other.seq
seqc other.seq
expect_status 7
expect_err "the entry point must be <project>/src/main.seq"
seqc src/missing.seq
expect_status 3
expect_err "error[E0110]"

# --- Bad settings. ------------------------------------------------------------
seqc --set limits.cpu_seconds=forever src/main.seq
expect_status 2
expect_err "must be a nonnegative integer"
seqc --set no.such=1 src/main.seq
expect_status 2
expect_err "unknown setting 'no.such'"
mkdir -p "$XDG_CONFIG_HOME/seqc"
echo 'this is not toml' >"$XDG_CONFIG_HOME/seqc/config.toml"
seqc src/main.seq
expect_status 2
expect_err "config.toml: line 1"

# --- Settings precedence: flag, then environment, then file, then default. ---
cat >"$XDG_CONFIG_HOME/seqc/config.toml" <<'EOF'
# Written by the integration test.
[build]
repair_attempts = 0
EOF
script broken --plan "$W/hello/plan.json" \
  --generate "$W/hello/steps_syntax_error.c" "$W/hello/steps.c" \
  --test "$W/hello/tests.cc"
seqc src/main.seq # file: no repair allowed
expect_status 5
expect_err "not accepted after 1 attempt(s)"
SEQC_BUILD_REPAIR_ATTEMPTS=1 "$SEQC" src/main.seq >"$OUT" 2>"$ERR"
STATUS=$? # environment beats the file
expect_status 0
expect_out "[repair]   attempt 2 of 2"
SEQC_BUILD_REPAIR_ATTEMPTS=1 "$SEQC" --set build.repair_attempts=0 \
  src/main.seq >"$OUT" 2>"$ERR"
STATUS=$? # flag beats the environment
expect_status 5
rm "$XDG_CONFIG_HOME/seqc/config.toml"

# --- seqc doctor. -------------------------------------------------------------
seqc doctor
expect_out "[ ok ] platform: Linux x86_64"
expect_out "[ ok ] sandbox: Landlock ABI"
expect_out "[ ok ] C compiler:"
expect_out "[ ok ] static C library:"
expect_out "[ ok ] Google Test: $SEQ_MINIGTEST_ROOT"
expect_out "[warn] inference runtime: the fake adapter is selected"
expect_status 0
expect_out "Execution is possible on this host."

seqc doctor --set model.adapter=llama-server --set model.llama_server=no-such-llama
expect_status 8
expect_out "[FAIL] inference runtime: 'no-such-llama' was not found"
expect_out "[FAIL] model: not locked for this project"
expect_out "Execution is not possible on this host yet: 2 item(s)"

# --help and --version work with nothing configured at all.
SEQC_HOME=/nonexistent "$SEQC" --version >"$OUT" 2>"$ERR"
STATUS=$?
expect_status 0
expect_out "grammar version: 1"
expect_out "prompt version: 1"
SEQC_HOME=/nonexistent "$SEQC" --help >"$OUT" 2>"$ERR"
STATUS=$?
expect_status 0
expect_out "seqc model pull"

echo "ok"
