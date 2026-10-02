#!/bin/bash
# Unusable model responses produce actionable failures, and an invalid
# workflow is rejected before the model is ever contacted.
. "$SEQ_INTEGRATION/lib.sh"

GOOD="$W/files/steps.c"
TESTS="$W/files/tests.cc"
PLAN="$W/files/plan.json"

# Writes a variant of the files plan: plan_variant NAME PYTHON-STATEMENTS
plan_variant() {
  "$PYTHON" - "$PLAN" "$SEQ_WORK/$1.json" "$2" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
exec(sys.argv[3])
json.dump(d, open(sys.argv[2], "w"))
EOF
}

project files files

# --- Validation happens before model access. ----------------------------------
script good --plan "$PLAN" --generate "$GOOD" --test "$TESTS"
cp src/main.seq "$SEQ_WORK/main.seq.good"
echo 'backend.Rust()' >>src/main.seq
seqc src/main.seq
expect_status 3
expect_err "error[E0304]"
expect_err "nothing was built"
expect_model_calls ""
expect_no_file output/temp/runs
cp "$SEQ_WORK/main.seq.good" src/main.seq

# --- A response that is not JSON. ---------------------------------------------
printf 'Sure! Here is the plan you asked for.' >"$SEQ_WORK/chatty.raw"
script chatty --plan "$SEQ_WORK/chatty.raw"
seqc src/main.seq
expect_status 4
expect_err "the model returned an unusable plan: the plan is not valid JSON"
expect_model_calls "plan"
# The request and the response are on record.
RUN=$(latest_run)
expect_file "${RUN}attempts/plan.request.json"
expect_content "${RUN}attempts/plan.response.txt" \
  "Sure! Here is the plan you asked for."

# --- A plan that omits, duplicates, or reorders a step. -----------------------
reset_model_calls
plan_variant omitted 'd["steps"].pop()'
script omitted --plan "$SEQ_WORK/omitted.json"
seqc src/main.seq
expect_status 4
expect_err "the plan has 1 step(s) but the workflow has 2"
expect_model_calls "plan"

plan_variant reordered 'd["steps"].reverse()'
script reordered --plan "$SEQ_WORK/reordered.json"
seqc src/main.seq
expect_status 4
expect_err "steps may not be omitted, duplicated, or reordered"

plan_variant duplicated 'd["steps"][1] = d["steps"][0]'
script duplicated --plan "$SEQ_WORK/duplicated.json"
seqc src/main.seq
expect_status 4
expect_err "steps may not be omitted, duplicated, or reordered"

# --- A plan that needs an unsupported library is not repaired. ----------------
reset_model_calls
plan_variant libpng 'd["dependencies"].append("libpng")'
script libpng --plan "$SEQ_WORK/libpng.json" --plan "$PLAN" \
  --generate "$GOOD" --test "$TESTS"
seqc src/main.seq
expect_status 4
expect_err 'the plan requires unsupported dependency "libpng"'
expect_err "never installs packages a model asks for"
expect_model_calls "plan"

# --- A plan that would write outside output/ or into the compiler's area. ----
plan_variant escape 'd["outputs"][0]["path"] = "../escape.txt"'
script escape --plan "$SEQ_WORK/escape.json"
seqc src/main.seq
expect_status 4
expect_err "plan output path is not acceptable"
plan_variant temp 'd["outputs"][0]["path"] = "temp/files.bin"'
script temp --plan "$SEQ_WORK/temp.json"
seqc src/main.seq
expect_status 4
expect_err "plan output path is not acceptable"

# --- A generation response without the source field. --------------------------
reset_model_calls
printf '{"code": "int x;"}' >"$SEQ_WORK/wrong-field.raw"
script wrong-field --plan "$PLAN" --generate "$SEQ_WORK/wrong-field.raw"
seqc src/main.seq
expect_status 4
expect_err 'the response has no string member "step_functions"'
expect_model_calls "plan generate"

# --- A provider failure, such as a timeout. -----------------------------------
reset_model_calls
script provider --plan "$PLAN" \
  --generate "error:inference timed out after 900 seconds"
seqc src/main.seq
expect_status 4
expect_err "inference failed (generate): inference timed out after 900 seconds"
expect_no_file output/temp/files.bin

# --- A script that runs out is a model failure, not a crash. ------------------
script short --plan "$PLAN" --generate "$GOOD"
seqc src/main.seq
expect_status 4
expect_err "inference failed (test)"

# --- Context overflow fails before inference, with nothing truncated. --------
reset_model_calls
script good2 --plan "$PLAN" --generate "$GOOD" --test "$TESTS"
seqc --set model.context_tokens=300 --set model.max_output_tokens=200 src/main.seq
expect_status 4
expect_err "exceeds the model window of 300"
expect_err "nothing is truncated silently"
expect_model_calls ""

# --- An oversized response is rejected. ---------------------------------------
"$PYTHON" -c 'print("{\"step_functions\": \"" + "/* pad */ " * 150000 + "\"}", end="")' \
  >"$SEQ_WORK/huge.raw"
script huge --plan "$PLAN" --generate "$SEQ_WORK/huge.raw"
seqc src/main.seq
expect_status 4
expect_err "larger than the limit of 512.0 KiB"

# --- Adapter configuration problems. ------------------------------------------
export SEQC_MODEL_FAKE_SCRIPT="$SEQ_WORK/does-not-exist.json"
seqc src/main.seq
expect_status 4
expect_err "file not found"

seqc --set model.adapter=telepathy src/main.seq
expect_status 4
expect_err "unknown model adapter 'telepathy'"

# The real adapter needs a locked model: nothing is downloaded implicitly and
# no other model is substituted.
seqc --set model.adapter=llama-server src/main.seq
expect_status 4
expect_err "the model is not locked for this project; run \`seqc model pull\` first"
expect_no_file "$XDG_CACHE_HOME/seqc/models"

echo "ok"
