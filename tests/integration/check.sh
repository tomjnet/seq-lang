#!/bin/bash
# `seqc check` validates a workflow with no model, no compiler, and no
# project around it, and prints diagnostics in the documented format.
. "$SEQ_INTEGRATION/lib.sh"

L="$SEQ_FIXTURES/language"

# Neither a model nor a compiler is available to this test.
unset SEQC_MODEL_ADAPTER SEQC_MODEL_FAKE_LOG SEQC_TOOLCHAIN_GTEST_ROOT
export SEQC_TOOLCHAIN_CC=/nonexistent/gcc SEQC_TOOLCHAIN_CXX=/nonexistent/g++
export SEQC_MODEL_LLAMA_SERVER=/nonexistent/llama-server

# --- Valid workflows. ---------------------------------------------------------
seqc check "$L/valid/reference.seq"
expect_status 0
expect_out "reference.seq: ok"
expect_out "  name:    top3Company"
expect_out "  model:   https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct"
expect_out "  steps:   3 (3 requests)"
expect_out "    1. step1 (1)"
expect_out "    3. step3 (1)"
[ ! -s "$ERR" ] || fail "a valid file produced diagnostics"

seqc check "$L/valid/comments_and_escapes.seq"
expect_status 0
expect_out "  name:    notes_demo-1"
expect_out "  steps:   2 (3 requests)"

# CRLF line endings and a byte-order mark give the same result as LF.
cp "$L/valid/reference.seq" lf.seq
sed 's/$/\r/' lf.seq >crlf.seq
{ printf '\xEF\xBB\xBF'; cat lf.seq; } >bom.seq
{ printf '\xEF\xBB\xBF'; cat crlf.seq; } >bom-crlf.seq
seqc check lf.seq
expect_status 0
sed 's/^lf.seq/FILE/' "$OUT" >expected.txt
for variant in crlf bom bom-crlf; do
  seqc check "$variant.seq"
  expect_status 0
  sed "s/^$variant.seq/FILE/" "$OUT" | cmp -s - expected.txt ||
    fail "$variant.seq does not check like its LF equivalent"
done

# --- Invalid workflows: exact diagnostics. ------------------------------------
# Each .expected file holds the exact stderr, with the path spelled FILE.
for source in "$L"/invalid/*.seq; do
  name=$(basename "$source" .seq)
  cp "$source" "$name.seq"
  seqc check "$name.seq"
  expect_status 3
  [ ! -s "$OUT" ] || fail "$name: diagnostics must go to stderr only"
  sed "s/$name\.seq/FILE/g" "$ERR" >"$name.actual"
  if ! cmp -s "$name.actual" "$L/invalid/$name.expected"; then
    diff -u "$L/invalid/$name.expected" "$name.actual" >&2
    fail "$name: diagnostics differ from $name.expected"
  fi
done

# The same errors stop a build before anything else happens.
mkdir -p proj/src
cp "$L/invalid/rust_backend.seq" proj/src/main.seq
seqc proj/src/main.seq
expect_status 3
expect_err "error[E0305]: backend.Rust() is not supported in v0.1"
expect_no_file proj/output

# --- Other input problems. ----------------------------------------------------
seqc check missing.seq
expect_status 3
expect_err "missing.seq:1:1: error[E0110]: file not found: missing.seq"

: >empty.seq
seqc check empty.seq
expect_status 3
expect_err "empty.seq:1:1: error[E0301]: missing required model declaration"
expect_err "error[E0302]"
expect_err "error[E0303]"
expect_err "3 error(s)"

printf 'name = "x"\n\xff\n' >binary.seq
seqc check binary.seq
expect_status 3
expect_err "binary.seq:2:1: error[E0101]: source file is not valid UTF-8"

# --- Color. -------------------------------------------------------------------
# Output is not a terminal here, so there are no escape sequences with or
# without NO_COLOR.
unset NO_COLOR
seqc check "$L/invalid/rust_backend.seq"
if grep -q $'\033' "$ERR"; then fail "color codes written to a pipe"; fi
export NO_COLOR=1

# --- Command line. ------------------------------------------------------------
seqc check
expect_status 2
seqc check a.seq b.seq
expect_status 2
seqc check --rebuild "$L/valid/reference.seq"
expect_status 2
seqc
expect_status 2
expect_err "Usage:"
seqc --no-such-flag
expect_status 2
expect_err "unknown option '--no-such-flag'"
seqc notes.txt
expect_status 2
expect_err "is not a command or a .seq source file"
seqc --settings
expect_status 0
expect_out "limits.cpu_seconds"
expect_out "model.adapter"

echo "ok"
