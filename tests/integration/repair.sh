#!/bin/bash
# Policy rejections, compile errors, and link errors go through bounded
# repair. An unrepairable program stops after exactly the permitted attempts
# and never runs a stale executable.
. "$SEQ_INTEGRATION/lib.sh"

project files files
PLAN="$W/files/plan.json"
GOOD="$W/files/steps.c"
TESTS="$W/files/tests.cc"

# --- A compile error is repaired on the second attempt. ----------------------
script syntax --plan "$PLAN" --generate "$W/files/steps_syntax.c" "$GOOD" \
  --test "$TESTS"
seqc src/main.seq
expect_status 0
expect_out "[generate] attempt 1 of 3"
expect_out "[compile]  failed"
expect_out "undeclared_variable"
expect_out "[repair]   attempt 2 of 3"
expect_out "[compile]  ok"
expect_out "sum=15"
expect_model_calls "plan generate repair test"
expect_json output/temp/build.json 'd["attempts"]["program"]' 2
RUN=$(latest_run)
# Both attempts and their diagnostics are kept.
expect_file "${RUN}attempts/generate-1/files.c"
expect_file "${RUN}attempts/generate-2/files.bin"
grep -q "undeclared_variable" "${RUN}attempts/generate-1/gcc.log" ||
  fail "the failed attempt's diagnostics were not kept"
# The repair request carries the diagnostics, the rejected source, and the
# original workflow.
REQUEST="${RUN}attempts/generate-2.request.json"
grep -q "undeclared_variable" "$REQUEST" || fail "no diagnostics in the repair"
grep -q "PREVIOUS ATTEMPT" "$REQUEST" || fail "no previous source in the repair"
grep -q "read numbers.txt and write their sum" "$REQUEST" ||
  fail "no workflow in the repair"
expect_json "$REQUEST" 'd["kind"]' repair

# --- A link error is repaired too. --------------------------------------------
reset_model_calls
script link --plan "$PLAN" --generate "$W/files/steps_link.c" "$GOOD" \
  --test "$TESTS"
seqc --rebuild src/main.seq
expect_status 0
expect_out "[link]     failed"
expect_out "function_that_does_not_exist"
expect_model_calls "plan generate repair test"

# --- Policy violations are reported with the offending line and repaired. ----
for case in main include missing; do
  reset_model_calls
  script "policy-$case" --plan "$PLAN" --generate "$W/files/steps_$case.c" \
    "$GOOD" --test "$TESTS"
  seqc --rebuild src/main.seq
  expect_status 0
  expect_out "[policy]   rejected the generated source"
  expect_model_calls "plan generate repair test"
  case $case in
  main) expect_out "main must not be defined" && expect_out "int main(void) {" ;;
  include) expect_out "header <unistd.h> is not on the include allowlist" &&
    expect_out "policy: line 1:" ;;
  missing) expect_out "required function seq_step_2 is not defined" ;;
  esac
  # A rejected source is never handed to the compiler.
  expect_no_file "$(latest_run)attempts/generate-1/gcc.log"
done

# --- Two repairs are allowed, so the third attempt may still succeed. --------
reset_model_calls
script third --plan "$PLAN" --generate "$W/files/steps_syntax.c" \
  "$W/files/steps_main.c" "$GOOD" --test "$TESTS"
seqc --rebuild src/main.seq
expect_status 0
expect_model_calls "plan generate repair repair test"
expect_json output/temp/build.json 'd["attempts"]["program"]' 3
ACCEPTED_KEY=$(json_get output/temp/build.json 'd["cache_key"]')
ACCEPTED_HASH=$(sha256sum output/temp/files.bin | cut -d' ' -f1)
cp output/total.txt "$SEQ_WORK/total-before.txt"

# --- Unrepairable: exactly three attempts, then stop. -------------------------
reset_model_calls
script hopeless --plan "$PLAN" --generate "$W/files/steps_syntax.c" \
  "$W/files/steps_syntax.c" "$W/files/steps_syntax.c" "$GOOD" --test "$TESTS"
seqc src/main.seq
expect_status 5
expect_err "the generated program was not accepted after 3 attempt(s)"
expect_err "1 generation and 2 repair(s)"
expect_model_calls "plan generate repair repair"
expect_out "[repair]   attempt 3 of 3"
expect_no_out "attempt 4"
# Nothing ran, and the last accepted build is untouched.
expect_no_out "---- program output ----"
expect_no_out "sum=15"
expect_json output/temp/build.json 'd["cache_key"]' "$ACCEPTED_KEY"
[ "$(sha256sum output/temp/files.bin | cut -d' ' -f1)" = "$ACCEPTED_HASH" ] ||
  fail "a failed build replaced the accepted binary"
cmp -s output/total.txt "$SEQ_WORK/total-before.txt" ||
  fail "a failed build changed published outputs"
# All three attempts are on record.
RUN=$(latest_run)
for attempt in 1 2 3; do
  expect_file "${RUN}attempts/generate-$attempt/gcc.log"
done
expect_no_file "${RUN}run-manifest.json"

# --- The repair limit is configurable. ----------------------------------------
reset_model_calls
seqc --set build.repair_attempts=0 src/main.seq
expect_status 5
expect_err "not accepted after 1 attempt(s)"
expect_model_calls "plan generate"

# --- In a project with no accepted build, a failed build leaves no binary. ---
project fresh files
reset_model_calls
seqc src/main.seq
expect_status 5
expect_no_file output/temp/fresh.bin
expect_no_file output/temp/fresh.c
expect_no_file output/temp/build.json
expect_no_file output/total.txt

echo "ok"
