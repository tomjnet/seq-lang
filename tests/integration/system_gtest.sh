#!/bin/bash
# Generated tests built against the installed Google Test, not the stand-in
# the other integration tests use. Skipped (exit status 77) on a machine
# without Google Test.
if [ ! -e /usr/include/gtest/gtest.h ]; then
  echo "Google Test is not installed system-wide; skipping"
  exit 77
fi
SEQ_SYSTEM_GTEST=1
. "$SEQ_INTEGRATION/lib.sh"

PLAN="$W/files/plan.json"
GOOD="$W/files/steps.c"

project files files

seqc doctor
expect_out "[ ok ] Google Test: found on the default search path"

# --- A test file with a compile error is repaired, then run. ------------------
script repair --plan "$PLAN" --generate "$GOOD" \
  --test "$W/files/tests_broken.cc" "$W/files/tests.cc"
seqc src/main.seq
expect_status 0
expect_out "the test file did not compile"
expect_out "[test]     repairing tests, attempt 2 of 3"
expect_out "[test]     Files.StepsRunInOrder: passed"
expect_out "[test]     Files.TotalIsFifteen: passed"
expect_out "[test]     2 passed, 0 failed"
expect_out "sum=15"
expect_model_calls "plan generate test test-repair"
RUN=$(latest_run)
# Linked against the system library, statically, with no stand-in prefix.
LOG="${RUN}attempts/test-2/g++.log"
grep -q -- "-static .*-lgtest_main -lgtest -lpthread" "$LOG" ||
  fail "unexpected test link command"
if grep -q minigtest "$LOG"; then fail "the stand-in was used"; fi
file output/temp/test/files_test.bin | grep -q "statically linked" ||
  fail "the test binary is not static"
# It is Google Test's own main that ran, inside the sandbox.
grep -q "Running main() from" "${RUN}logs/test-stdout.log" ||
  fail "gtest_main did not run"
expect_json output/temp/build.json 'd["tests"]["status"]' completed
expect_json output/temp/build.json 'd["tests"]["passed"]' 2
# The workflow executable never links Google Test.
if grep -q gtest "${RUN}attempts/generate-1/link.log"; then
  fail "the workflow executable was linked against Google Test"
fi

# --- A failing case is reported with Google Test's own detail. ----------------
reset_model_calls
script failing --plan "$PLAN" --generate "$GOOD" \
  --test "$W/files/tests_failing.cc"
seqc --rebuild src/main.seq
expect_status 0
expect_out "[test]     Files.TotalIsSixteen: FAILED"
expect_out "[test]     1 passed, 1 failed"
expect_out "sum=15"
grep -q "Expected equality of these values" "$(latest_run)logs/test-stdout.log" ||
  fail "the test log lacks Google Test's failure detail"
expect_json output/temp/build.json 'd["tests"]["cases"][1]["passed"]' False

# --- A crashing test binary is recorded as such. ------------------------------
script crash --plan "$PLAN" --generate "$GOOD" --test "$W/files/tests_crash.cc"
seqc --rebuild src/main.seq
expect_status 0
expect_out "the test binary did not finish"
expect_json output/temp/build.json 'd["tests"]["status"]' abnormal
expect_json output/temp/build.json 'd["tests"]["passed"]' 1

# --- Death tests are rejected before the compiler sees them. ------------------
script death --plan "$PLAN" --generate "$GOOD" \
  --test "$W/files/tests_death.cc" "$W/files/tests.cc"
seqc --rebuild src/main.seq
expect_status 0
expect_out "death tests are not allowed"

# --- The chart workflow's tests pass against the real library too. ------------
project top3Company chart
script chart --plan "$W/chart/plan.json" --generate "$W/chart/steps.c" \
  --test "$W/chart/tests.cc"
seqc src/main.seq
expect_status 0
expect_out "[test]     3 passed, 0 failed"
expect_file output/top3.png

echo "ok"
