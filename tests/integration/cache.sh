#!/bin/bash
# An unchanged project reuses its accepted build without inference; each part
# of the cache key invalidates it; --rebuild forces synthesis.
. "$SEQ_INTEGRATION/lib.sh"

project files files
echo "first input" >input/data.txt
script files --plan "$W/files/plan.json" --generate "$W/files/steps.c" \
  --test "$W/files/tests.cc"

seqc src/main.seq
expect_status 0
expect_out "[cache]    building: no accepted build yet"
expect_out "sum=15"
expect_model_calls "plan generate test"
FIRST_KEY=$(json_get output/temp/build.json 'd["cache_key"]')
FIRST_HASH=$(sha256sum output/temp/files.bin | cut -d' ' -f1)

# Unchanged: no inference, no compilation, same binary, results reused.
reset_model_calls
seqc src/main.seq
expect_status 0
expect_out "[cache]    reusing the accepted build"
expect_out "[test]     recorded results: 2 passed, 0 failed"
expect_no_out "[generate]"
expect_no_out "[compile]"
expect_out "sum=15"
expect_model_calls ""
[ "$(sha256sum output/temp/files.bin | cut -d' ' -f1)" = "$FIRST_HASH" ] ||
  fail "the cached binary changed"
expect_json "$(latest_run)run-manifest.json" 'd["reused_cached_build"]' True
expect_content output/total.txt 15

# The key covers the normalized source, so saving the file with CRLF line
# endings does not invalidate the build.
sed -i 's/$/\r/' src/main.seq
reset_model_calls
seqc src/main.seq
expect_status 0
expect_out "reusing the accepted build"
expect_model_calls ""
sed -i 's/\r$//' src/main.seq

rebuilds_after() {
  reset_model_calls
  seqc src/main.seq
  expect_status 0
  expect_out "[cache]    building: the workflow, its inputs, the model, or the toolchain changed"
  expect_model_calls "plan generate test"
  local key
  key=$(json_get output/temp/build.json 'd["cache_key"]')
  [ "$key" != "$1" ] || fail "the cache key did not change after: $2"
  LAST_KEY=$key
}

# The source.
sed -i 's/also print the sum/also print the sum please/' src/main.seq
rebuilds_after "$FIRST_KEY" "a source edit"

# An input file's content, and a new input file.
echo "second input" >input/data.txt
rebuilds_after "$LAST_KEY" "an input edit"
echo "more" >input/extra.txt
rebuilds_after "$LAST_KEY" "a new input"

# The locked model.
cat >seq.lock <<EOF
{
  "lock_version": 1,
  "declared_url": "https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct",
  "declared_revision": "",
  "repo": "Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF",
  "revision": "1111111111111111111111111111111111111111",
  "file": "model.gguf",
  "quantization": "q4_k_m",
  "sha256": "$(printf 'a%.0s' $(seq 64))",
  "grammar_version": 1,
  "prompt_version": 1
}
EOF
rebuilds_after "$LAST_KEY" "locking a model"
sed -i 's/1111111111111111111111111111111111111111/2222222222222222222222222222222222222222/' seq.lock
rebuilds_after "$LAST_KEY" "a new locked revision"

# Generation settings.
export SEQC_MODEL_SEED=7
rebuilds_after "$LAST_KEY" "a new seed"

# The scripted model itself (its identity is the script's hash).
script files2 --plan "$W/files/plan.json" --generate "$W/files/steps.c" \
  --test "$W/files/tests.cc" --test "$W/files/tests.cc"
rebuilds_after "$LAST_KEY" "a different model"

# Settled again.
reset_model_calls
seqc src/main.seq
expect_status 0
expect_out "reusing the accepted build"
expect_model_calls ""

# --rebuild ignores a valid cache.
seqc --rebuild src/main.seq
expect_status 0
expect_out "[cache]    building: --rebuild was given"
expect_model_calls "plan generate test"

# A lock written for another model is refused, not silently used.
sed -i 's|Qwen2.5-Coder-1.5B-Instruct"|Some-Other-Model"|' seq.lock
seqc src/main.seq
expect_status 4
expect_err "seq.lock was written for"
sed -i 's|Some-Other-Model"|Qwen2.5-Coder-1.5B-Instruct"|' seq.lock
seqc src/main.seq
expect_status 0

# A build artifact that changed after it was accepted is never run: the
# project is rebuilt instead.
reset_model_calls
echo "tampered" >>output/temp/files.bin
seqc src/main.seq
expect_status 0
expect_out "[cache]    building: build artifact changed since it was accepted: output/temp/files.bin"
expect_model_calls "plan generate test"

reset_model_calls
rm output/temp/files.s
seqc src/main.seq
expect_status 0
expect_out "[cache]    building: build artifact is missing: output/temp/files.s"
expect_file output/temp/files.s

echo "ok"
