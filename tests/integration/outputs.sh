#!/bin/bash
# Output validation, staged publication, the rerun rule, and --force.
. "$SEQ_INTEGRATION/lib.sh"

project files files
PLAN="$W/files/plan.json"
TESTS="$W/files/tests.cc"
script files --plan "$PLAN" --generate "$W/files/steps.c" --test "$TESTS"

# --- Declared outputs are published and listed, final ones marked. -----------
seqc src/main.seq
expect_status 0
expect_out "Results:"
expect_out "  output/numbers.txt  (10 bytes)"
expect_out "  output/total.txt  (3 bytes)  final"
expect_content output/total.txt 15
expect_content output/numbers.txt "1
2
3
4
5"
RUN=$(latest_run)
M="${RUN}run-manifest.json"
expect_json "$M" 'len(d["published"])' 2
expect_json "$M" 'd["published"][1]["path"]' total.txt
expect_json "$M" 'd["published"][1]["final"]' True
expect_json "$M" 'd["published"][1]["sha256"]' \
  "$(sha256sum output/total.txt | cut -d' ' -f1)"
expect_json "$M" 'd["undeclared"]' "[]"
# Published files moved out of staging; nothing is left behind there.
expect_no_file "${RUN}staging/total.txt"
expect_file output/temp/published.json

# --- Rerun: seqc replaces the files it published itself. ----------------------
seqc src/main.seq
expect_status 0
expect_content output/total.txt 15

# --- A file the user edited is not replaced. ----------------------------------
echo "my own notes" >output/total.txt
seqc src/main.seq
expect_status 7
expect_err "output/total.txt was modified after seqc created it"
expect_err "--force"
expect_content output/total.txt "my own notes"
# Nothing at all was published: numbers.txt would have been replaceable.
expect_json "$(latest_run)run-manifest.json" 'd["status"]' publish-refused
expect_json "$(latest_run)run-manifest.json" 'd["published"]' "[]"

seqc --force src/main.seq
expect_status 0
expect_content output/total.txt 15

# --- A file the user created is not replaced either. --------------------------
project fresh files
echo "precious" >output/numbers.txt
seqc src/main.seq
expect_status 7
expect_err "output/numbers.txt already exists and was not created by seqc"
expect_content output/numbers.txt precious
expect_no_file output/total.txt
seqc --force src/main.seq
expect_status 0
expect_content output/total.txt 15
[ "$(head -1 output/numbers.txt)" = 1 ] || fail "--force did not replace"

# A directory in the way is refused even with --force.
project blocked files
mkdir output/total.txt
seqc --force src/main.seq
expect_status 7
expect_err "a directory with that name exists"
[ -d output/total.txt ] || fail "the directory was removed"
expect_no_file output/numbers.txt

# --- `seqc clean` keeps the record of published files, so reruns still work. --
cd "$SEQ_WORK/files" || exit 1
seqc clean --all
expect_status 0
expect_content output/total.txt 15
seqc src/main.seq
expect_status 0
expect_content output/total.txt 15

# --- A missing declared output fails the run and publishes nothing. ----------
project missing files
script no-total --plan "$PLAN" --generate "$W/files/steps_no_total.c" \
  --test "$TESTS"
seqc src/main.seq
expect_status 6
expect_err "declared output was not produced: total.txt"
expect_err "nothing was published"
expect_no_file output/numbers.txt
expect_no_file output/total.txt
expect_json "$(latest_run)run-manifest.json" 'd["status"]' invalid-outputs
# What the program did write is kept in the run directory for inspection.
expect_file "$(latest_run)staging/numbers.txt"

# --- So does an empty one. ----------------------------------------------------
project empty files
script empty-total --plan "$PLAN" --generate "$W/files/steps_empty_total.c" \
  --test "$TESTS"
seqc src/main.seq
expect_status 6
expect_err "declared output is empty: total.txt"
expect_no_file output/numbers.txt

# --- And one that is not what the plan says it is. ---------------------------
project wrongkind files
script not-text --plan "$PLAN" --generate "$W/files/steps_not_text.c" \
  --test "$TESTS"
seqc src/main.seq
expect_status 6
expect_err "declared text output is not valid UTF-8: total.txt"
expect_no_file output/total.txt

# --- An undeclared file is kept, reported, and not published. ----------------
project extra files
script extra --plan "$PLAN" --generate "$W/files/steps_extra.c" --test "$TESTS"
seqc src/main.seq
expect_status 0
expect_out "not published (the plan did not declare it): scratch/notes.txt"
expect_no_file output/scratch/notes.txt
expect_no_file output/scratch
expect_file "$(latest_run)staging/scratch/notes.txt"
expect_json "$(latest_run)run-manifest.json" 'd["undeclared"]' \
  "['scratch/notes.txt']"
expect_content output/total.txt 15

# --- A chart workflow publishes a valid PNG as its final artifact. -----------
project top3Company chart
script chart --plan "$W/chart/plan.json" --generate "$W/chart/steps.c" \
  --test "$W/chart/tests.cc"
seqc src/main.seq
expect_status 0
expect_out "ACME 385.00"
expect_out "Globex 480.00"
expect_out "Initech 292.50"
expect_out "Umbrella 80.00"
expect_out "[test]     3 passed, 0 failed"
expect_out "  output/company.txt"
expect_out "  output/top3.png"
grep -q "output/top3.png .*final" "$OUT" || fail "top3.png is not marked final"
[ "$(wc -l <output/company.txt)" -eq 5 ] || fail "company.txt lacks 5 records"
"$PYTHON" "$SEQ_SOURCE_DIR/tests/runtime/check_png.py" output/top3.png 800 480 3 \
  >/dev/null || fail "top3.png does not decode as an 800x480 chart with 3 bars"

echo "ok"
