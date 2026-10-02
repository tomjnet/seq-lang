#!/bin/bash
# The starter project builds, runs, and prints "Hello from Seq".
. "$SEQ_INTEGRATION/lib.sh"

project hello hello
script hello --plan "$W/hello/plan.json" --generate "$W/hello/steps.c" \
  --test "$W/hello/tests.cc"

seqc src/main.seq
expect_status 0
expect_out "Hello from Seq"
expect_out "[check]"
expect_out "1 step(s), 1 request(s), backend C"
expect_out "[plan]"
expect_out "[generate] attempt 1 of 3"
expect_out "[policy]   ok"
expect_out "[compile]  ok"
expect_out "[link]     ok (built from the emitted assembly)"
expect_out "[test]     Hello.StepSucceeds: passed"
expect_out "[test]     2 passed, 0 failed"
expect_out "[build]    accepted"
expect_out "[run]      step 1 (step1): ok"
expect_out "Results: none"
expect_model_calls "plan generate test"

# Every accepted build has all five artifacts.
for file in hello.c hello.s hello.bin test/hello_test.cc test/hello_test.bin; do
  expect_file "output/temp/$file"
done
[ -x output/temp/hello.bin ] || fail "hello.bin is not executable"
file output/temp/hello.bin | grep -q "x86-64.*statically linked" ||
  fail "hello.bin is not a static x86-64 executable"

# The source is the compiler-owned prelude, the model's step function, and
# the guarded driver.
grep -q '#include "seq_runtime.h"' output/temp/hello.c || fail "no prelude"
grep -q 'int seq_step_1(seq_ctx \*ctx) {' output/temp/hello.c || fail "no step"
grep -q '#ifndef SEQ_NO_MAIN' output/temp/hello.c || fail "main is not guarded"
grep -q 'seq_step_1' output/temp/hello.s || fail "assembly lacks the step"
grep -q 'extern "C"' output/temp/test/hello_test.cc || fail "no test prelude"

# The build manifest records provenance.
B=output/temp/build.json
expect_json $B 'd["workflow"]' hello
expect_json $B 'd["attempts"]["program"]' 1
expect_json $B 'd["attempts"]["tests"]' 1
expect_json $B 'd["tests"]["passed"]' 2
expect_json $B 'd["tests"]["failed"]' 0
expect_json $B 'd["tests"]["status"]' completed
expect_json $B 'd["model"]["adapter"]' fake
expect_json $B 'd["generation"]["temperature"]' 0.0
expect_json $B 'len(d["cache_key"])' 64
expect_json $B 'len(d["artifacts"])' 5
expect_json $B 'd["plan"]["steps"][0]["name"]' step1
expect_json $B '"gcc" in d["toolchain"]["cc_version"]' True

# Run records: manifests, the plan, every attempt, and logs.
RUN=$(latest_run)
for file in input-manifest.json plan.json build-manifest.json run-manifest.json \
  logs/stdout.log logs/steps.log attempts/plan.request.json \
  attempts/generate-1.request.json attempts/generate-1/gcc.log \
  attempts/test-1.request.json; do
  expect_file "$RUN$file"
done
expect_content "${RUN}logs/stdout.log" "Hello from Seq"
expect_json "${RUN}run-manifest.json" 'd["status"]' ok
expect_json "${RUN}run-manifest.json" 'd["steps"][0]["status"]' ok
expect_json "${RUN}run-manifest.json" 'd["reused_cached_build"]' False
# The binary is linked from the emitted assembly, with -O3.
grep -q -- "-O3 .*-S hello.c -o hello.s" "${RUN}attempts/generate-1/gcc.log" ||
  fail "assembly was not emitted with -O3"
grep -q "hello.s -o hello.bin -static" "${RUN}attempts/generate-1/link.log" ||
  fail "the binary was not linked from the assembly"
# Run records are private to the user.
[ "$(stat -c %a "$RUN")" = 700 ] || fail "run directory is not private"
# The whole workflow reaches the model.
grep -q "print Hello from Seq" "${RUN}attempts/generate-1.request.json" ||
  fail "the generate request lacks the workflow"

# The generated executable also runs by hand, outside seqc.
[ "$(./output/temp/hello.bin)" = "Hello from Seq" ] ||
  fail "hello.bin does not run by hand"

# --quiet prints only program output and the result list.
seqc --quiet src/main.seq
expect_status 0
expect_content "$OUT" "Hello from Seq

Results: none (the workflow printed its result to the console)"

# --build-only stops before execution.
seqc --build-only --rebuild src/main.seq
expect_status 0
expect_out "stopping before execution"
expect_no_out "Hello from Seq"
expect_out "output/temp/hello.bin"

# The Makefile copied into output/temp rebuilds the program by hand when it
# is given the runtime paths.
HOME_DIR=$(dirname "$(dirname "$SEQC")")/lib/seqc
make -C output/temp INCLUDE_FLAGS="-I$HOME_DIR/include" \
  LINK_FLAGS="-static -L$HOME_DIR/lib" LINK_LIBS="-lseqrt -lm" \
  >"$OUT" 2>"$ERR" || fail "manual make failed"
[ "$(./output/temp/build/hello.bin)" = "Hello from Seq" ] ||
  fail "the hand-built program does not run"

echo "ok"
