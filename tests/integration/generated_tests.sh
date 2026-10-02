#!/bin/bash
# Every accepted build has a compiled Google Test file. A test file that does
# not compile is repaired; one that cannot be repaired fails the build.
# Failing cases are reported and recorded, and do not block execution.
. "$SEQ_INTEGRATION/lib.sh"

project files files
PLAN="$W/files/plan.json"
GOOD="$W/files/steps.c"

# --- A test file with a compile error is repaired. ----------------------------
script broken --plan "$PLAN" --generate "$GOOD" \
  --test "$W/files/tests_broken.cc" "$W/files/tests.cc"
seqc src/main.seq
expect_status 0
expect_out "[test]     generating tests, attempt 1 of 3"
expect_out "the test file did not compile"
expect_out "[test]     repairing tests, attempt 2 of 3"
expect_out "[test]     Files.TotalIsFifteen: passed"
expect_out "[test]     2 passed, 0 failed"
expect_model_calls "plan generate test test-repair"
expect_json output/temp/build.json 'd["attempts"]["tests"]' 2
expect_json output/temp/build.json 'd["attempts"]["program"]' 1
RUN=$(latest_run)
REQUEST="${RUN}attempts/test-2.request.json"
expect_json "$REQUEST" 'd["kind"]' test-repair
grep -q "function_that_was_never_declared" "$REQUEST" ||
  fail "no diagnostics in the test repair request"
# The test request sees the workflow, the plan, and the accepted C source.
FIRST="${RUN}attempts/test-1.request.json"
grep -q "read numbers.txt and write their sum" "$FIRST" || fail "no workflow"
grep -q "numbers.txt: the integers 1 to 5" "$FIRST" || fail "no plan"
grep -q "seq_output_open" "$FIRST" || fail "no step source"

# The test file has the compiler-owned prelude and the model's cases, and the
# test binary is linked with the program compiled as SEQ_NO_MAIN.
T=output/temp/test/files_test.cc
grep -q '#include <gtest/gtest.h>' $T || fail "no gtest include"
grep -q 'int seq_step_2(seq_ctx \*ctx);' $T || fail "no step declarations"
grep -q 'TEST(Files, TotalIsFifteen)' $T || fail "no model-written cases"
expect_file output/temp/test/files_test.bin
grep -q -- "-lgtest_main -lgtest" "${RUN}attempts/test-2/g++.log" ||
  fail "the test binary was not linked against Google Test"
expect_file "${RUN}attempts/test-2/files.o"
# Google Test is never linked into the workflow executable.
if grep -q gtest "${RUN}attempts/generate-1/link.log"; then
  fail "the workflow executable was linked against Google Test"
fi

# The tests ran in their own staging directory: their files never reach
# output/, which holds only what the real run published.
expect_file "${RUN}test-staging/total.txt"
expect_file "${RUN}logs/test-stdout.log"
expect_content output/total.txt 15

# --- Death tests are rejected by policy, then repaired. -----------------------
reset_model_calls
script death --plan "$PLAN" --generate "$GOOD" \
  --test "$W/files/tests_death.cc" "$W/files/tests.cc"
seqc --rebuild src/main.seq
expect_status 0
expect_out "[policy]   rejected the generated tests"
expect_out "death tests are not allowed"
expect_model_calls "plan generate test test-repair"

# --- Failing cases are reported and recorded; the workflow still runs. -------
reset_model_calls
script failing --plan "$PLAN" --generate "$GOOD" \
  --test "$W/files/tests_failing.cc"
seqc --rebuild src/main.seq
expect_status 0
expect_out "[test]     Files.StepsRunInOrder: passed"
expect_out "[test]     Files.TotalIsSixteen: FAILED"
expect_out "[test]     1 passed, 1 failed"
expect_out "sum=15"
B=output/temp/build.json
expect_json $B 'd["tests"]["passed"]' 1
expect_json $B 'd["tests"]["failed"]' 1
expect_json $B 'd["tests"]["cases"][1]["name"]' Files.TotalIsSixteen
expect_json $B 'd["tests"]["cases"][1]["passed"]' False
grep -q "Expected: (16) == (value)" "$(latest_run)logs/test-stdout.log" ||
  fail "the test log lacks the failure detail"

# A cache hit reuses the test file and its recorded results without running
# or inferring anything.
reset_model_calls
seqc src/main.seq
expect_status 0
expect_out "[test]     recorded results: 1 passed, 1 failed"
expect_model_calls ""
expect_no_file "$(latest_run)test-staging"

# --- A crashing test binary is reported, and the build is still accepted. ----
reset_model_calls
script crash --plan "$PLAN" --generate "$GOOD" --test "$W/files/tests_crash.cc"
seqc --rebuild src/main.seq
expect_status 0
expect_out "the test binary did not finish"
expect_json $B 'd["tests"]["status"]' abnormal
expect_json $B 'd["tests"]["passed"]' 1
expect_out "sum=15"

# --- An unrepairable test file fails the build after three attempts. ---------
ACCEPTED_KEY=$(json_get $B 'd["cache_key"]')
reset_model_calls
script hopeless --plan "$PLAN" --generate "$GOOD" \
  --test "$W/files/tests_broken.cc" "$W/files/tests_broken.cc" \
  "$W/files/tests_broken.cc" "$W/files/tests.cc"
seqc src/main.seq
expect_status 5
expect_err "the generated tests were not accepted after 3 attempt(s)"
expect_err "a build without a compiled test file is not accepted"
expect_model_calls "plan generate test test-repair test-repair"
expect_no_out "---- program output ----"
# The previous accepted build stays in place.
expect_json $B 'd["cache_key"]' "$ACCEPTED_KEY"

echo "ok"
