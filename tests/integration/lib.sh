# Shared helpers for the integration tests. Sourced, not executed.
#
# Each test drives the real seqc binary. Model responses come from the
# scripted fake adapter and generated tests are built against minigtest, so
# the tests need no model, no network, and no installed Google Test.
#
# Environment, set by tests/CMakeLists.txt:
#   SEQC                the seqc binary under test
#   SEQ_FIXTURES        tests/fixtures
#   SEQ_INTEGRATION     tests/integration
#   SEQ_MINIGTEST_ROOT  prefix of the Google Test stand-in
#   SEQ_SOURCE_DIR      repository root
#   SEQ_WORK            scratch directory owned by this test
#   PYTHON              python3

set -u

: "${SEQC:?}" "${SEQ_FIXTURES:?}" "${SEQ_INTEGRATION:?}" "${SEQ_WORK:?}"
: "${SEQ_MINIGTEST_ROOT:?}" "${SEQ_SOURCE_DIR:?}"
PYTHON=${PYTHON:-python3}

rm -rf "$SEQ_WORK"
mkdir -p "$SEQ_WORK"
cd "$SEQ_WORK" || exit 1

# Nothing from the developer's own configuration or cache may leak in.
export XDG_CONFIG_HOME="$SEQ_WORK/xdg-config"
export XDG_CACHE_HOME="$SEQ_WORK/xdg-cache"
export NO_COLOR=1
unset SEQC_HOME
for name in $(env | sed -n 's/^\(SEQC_[A-Z_]*\)=.*/\1/p'); do
  unset "$name"
done

export SEQC_MODEL_ADAPTER=fake
export SEQC_MODEL_FAKE_LOG="$SEQ_WORK/model-calls.log"
# A test that sets SEQ_SYSTEM_GTEST=1 before sourcing this file builds
# generated tests against the installed Google Test instead of the stand-in.
if [ "${SEQ_SYSTEM_GTEST:-0}" != 1 ]; then
  export SEQC_TOOLCHAIN_GTEST_ROOT="$SEQ_MINIGTEST_ROOT"
fi

OUT="$SEQ_WORK/stdout.txt"
ERR="$SEQ_WORK/stderr.txt"
STATUS=0

fail() {
  echo "FAIL: $*" >&2
  echo "---- last stdout ----" >&2
  cat "$OUT" >&2 2>/dev/null
  echo "---- last stderr ----" >&2
  cat "$ERR" >&2 2>/dev/null
  exit 1
}

# seqc ARGS...: runs seqc, keeping stdout, stderr, and the exit status.
seqc() {
  "$SEQC" "$@" >"$OUT" 2>"$ERR"
  STATUS=$?
}

expect_status() {
  [ "$STATUS" -eq "$1" ] || fail "expected exit status $1, got $STATUS"
}

expect_out() {
  grep -qF -- "$1" "$OUT" || fail "stdout does not contain: $1"
}

expect_no_out() {
  if grep -qF -- "$1" "$OUT"; then fail "stdout unexpectedly contains: $1"; fi
}

expect_err() {
  grep -qF -- "$1" "$ERR" || fail "stderr does not contain: $1"
}

expect_file() {
  [ -f "$1" ] || fail "expected file: $1"
}

expect_no_file() {
  if [ -e "$1" ] || [ -L "$1" ]; then fail "unexpected file: $1"; fi
}

expect_content() {
  [ "$(cat "$1")" = "$2" ] || fail "$1 holds '$(cat "$1")', expected '$2'"
}

# json_get FILE EXPRESSION: prints a value from a JSON file; `d` is the
# document, for example: json_get build.json 'd["attempts"]["program"]'
json_get() {
  "$PYTHON" -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$1" "$2"
}

expect_json() {
  local got
  got=$(json_get "$1" "$2") || fail "cannot read $2 from $1"
  [ "$got" = "$3" ] || fail "$1: $2 is '$got', expected '$3'"
}

# model_calls: the kinds of inference requests made so far, space separated.
model_calls() {
  if [ -f "$SEQC_MODEL_FAKE_LOG" ]; then
    tr '\n' ' ' <"$SEQC_MODEL_FAKE_LOG" | sed 's/ $//'
  fi
}

expect_model_calls() {
  [ "$(model_calls)" = "$1" ] ||
    fail "model calls were '$(model_calls)', expected '$1'"
}

reset_model_calls() {
  rm -f "$SEQC_MODEL_FAKE_LOG"
}

# script NAME ARGS...: builds a fake-adapter script and selects it.
script() {
  local name=$1
  shift
  "$PYTHON" "$SEQ_INTEGRATION/make_script.py" "$SEQ_WORK/$name.json" "$@" ||
    fail "cannot build model script $name"
  export SEQC_MODEL_FAKE_SCRIPT="$SEQ_WORK/$name.json"
}

# project NAME WORKFLOW: scaffolds a project and installs the fixture
# workflow's main.seq (if it has one). Leaves the shell inside the project.
project() {
  cd "$SEQ_WORK" || exit 1
  seqc new "$1"
  expect_status 0
  cd "$1" || exit 1
  if [ -f "$SEQ_FIXTURES/workflows/$2/main.seq" ]; then
    sed "s/@NAME@/$1/" "$SEQ_FIXTURES/workflows/$2/main.seq" >src/main.seq
  fi
}

# latest_run: path of the newest run directory of the current project.
latest_run() {
  ls -d output/temp/runs/*/ | sort | tail -1
}

W="$SEQ_FIXTURES/workflows"
