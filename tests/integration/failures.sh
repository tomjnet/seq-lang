#!/bin/bash
# Runtime failures: a failed step stops the workflow, limits terminate a
# runaway program, and the sandbox denies what generated code must not reach.
. "$SEQ_INTEGRATION/lib.sh"

PLAN="$W/files/plan.json"
# These programs are not meant to pass their tests; one trivial case keeps
# the build acceptable.
cat >"$SEQ_WORK/trivial_tests.cc" <<'EOF'
TEST(Trivial, Passes) { EXPECT_EQ(1, 1); }
EOF
TESTS="$SEQ_WORK/trivial_tests.cc"

# --- A failing step stops the workflow. ---------------------------------------
project files files
script fail --plan "$PLAN" --generate "$W/files/steps_fail.c" --test "$TESTS"
seqc src/main.seq
expect_status 6
expect_out "step one ran"
expect_no_out "step two ran"
expect_out "[run]      step 1 (make_numbers): failed"
expect_err "step 1 (make_numbers) failed with code 1: disk on fire, code 7"
expect_err "later steps were not run and nothing was published"
# The intermediate file of the failed step is not published.
expect_no_file output/numbers.txt
RUN=$(latest_run)
M="${RUN}run-manifest.json"
expect_json "$M" 'd["status"]' step-failed
expect_json "$M" 'd["exit_code"]' 70
expect_json "$M" 'len(d["steps"])' 1
expect_json "$M" 'd["steps"][0]["message"]' "disk on fire, code 7"
expect_content "${RUN}logs/steps.log" "begin 1 make_numbers
message 1 disk on fire, code 7
fail 1 make_numbers 1"

# The failure is not repaired automatically: a rerun reuses the same build
# and fails the same way, without any inference.
reset_model_calls
seqc src/main.seq
expect_status 6
expect_out "reusing the accepted build"
expect_model_calls ""

# --- An infinite loop is terminated by the CPU limit. -------------------------
project spin files
script spin --plan "$PLAN" --generate "$W/files/steps_spin.c" --test "$TESTS"
# Build first, so that only the run is timed.
seqc --build-only src/main.seq
expect_status 0
START=$(date +%s)
seqc --set limits.cpu_seconds=1 --set limits.wall_seconds=30 src/main.seq
ELAPSED=$(($(date +%s) - START))
expect_status 6
expect_out "before the loop"
expect_err "the program did not finish in step 2 (add_numbers)"
expect_err "CPU limit of 1 seconds"
[ "$ELAPSED" -lt 20 ] || fail "the runaway program ran for $ELAPSED seconds"
expect_json "$(latest_run)run-manifest.json" 'd["status"]' abnormal-end
expect_json "$(latest_run)run-manifest.json" 'd["steps"][1]["status"]' running

# The wall-clock limit catches it when the CPU limit is higher. The cached
# build is reused: limits are not part of the cache key.
reset_model_calls
seqc --set limits.cpu_seconds=25 --set limits.wall_seconds=1 src/main.seq
expect_status 6
expect_err "wall-clock limit of 1 seconds"
expect_model_calls ""

# --- Excessive output is cut off. ---------------------------------------------
project flood files
script flood --plan "$PLAN" --generate "$W/files/steps_flood.c" --test "$TESTS"
seqc --set limits.output_mb=1 src/main.seq
expect_status 6
expect_err "wrote more than 1 MiB to stdout and stderr"
[ "$(stat -c %s "$OUT")" -lt $((32 * 1024 * 1024)) ] ||
  fail "seqc relayed far more than the output limit"

# --- The sandbox denies reads and writes outside the program's areas. --------
project escape files
echo "input line" >input/data.txt
rm -f /tmp/seq-integration-escape.txt
script escape --plan "$PLAN" --generate "$W/files/steps_escape.c" \
  --test "$TESTS"
seqc src/main.seq
expect_status 0
expect_no_out "ALLOWED"
for line in \
  "read /etc/passwd: denied" \
  "read /proc/self/environ: denied" \
  "read ../../../../../src/main.seq: denied" \
  "read ../../../escape.c: denied" \
  "write ../../../escape.c: denied" \
  "write ../../../../planted.txt: denied" \
  "write /tmp/seq-integration-escape.txt: denied" \
  "write ../../../../../input/data.txt: denied" \
  "write ../../../../../input/planted.txt: denied" \
  "helper ../x: denied" \
  "helper /abs: denied" \
  "input: input line"; do
  expect_out "$line"
done
expect_content input/data.txt "input line"
expect_no_file input/planted.txt
expect_no_file output/planted.txt
expect_no_file /tmp/seq-integration-escape.txt
# The control: the same binary, run by hand without the sandbox, can read
# the host. The denials above come from seqc's isolation, not from the
# program.
(cd "$(latest_run)staging" && "$SEQ_WORK/escape/output/temp/escape.bin") \
  >"$SEQ_WORK/unsandboxed.txt" 2>&1
grep -q "read /etc/passwd: ALLOWED" "$SEQ_WORK/unsandboxed.txt" ||
  fail "the control run could not read /etc/passwd either"
rm -f /tmp/seq-integration-escape.txt

echo "ok"
