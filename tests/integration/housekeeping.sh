#!/bin/bash
# Templates in output/temp, the project lock, cleaning, and input rules.
. "$SEQ_INTEGRATION/lib.sh"

project files files
echo "input" >input/data.txt
script files --plan "$W/files/plan.json" --generate "$W/files/steps.c" \
  --test "$W/files/tests.cc"

# --- Templates are byte-identical copies of the single source in docs/. ------
cmp -s output/temp/.gitignore "$SEQ_SOURCE_DIR/docs/gitignore.template" ||
  fail ".gitignore differs from docs/gitignore.template"
cmp -s output/temp/Makefile "$SEQ_SOURCE_DIR/docs/Makefile.template" ||
  fail "Makefile differs from docs/Makefile.template"
[ -d output/temp/test ] || fail "output/temp/test is missing"

# A build restores a deleted template and leaves an edited one alone.
rm output/temp/.gitignore
echo "# my own rules" >output/temp/Makefile
seqc src/main.seq
expect_status 0
cmp -s output/temp/.gitignore "$SEQ_SOURCE_DIR/docs/gitignore.template" ||
  fail "the deleted .gitignore was not restored"
expect_content output/temp/Makefile "# my own rules"

# --- Only one seqc per project at a time. -------------------------------------
(
  flock 9
  touch "$SEQ_WORK/holding"
  sleep 30
) 9>output/temp/.seqc.lock &
HOLDER=$!
while [ ! -e "$SEQ_WORK/holding" ]; do sleep 0.05; done
START=$(date +%s)
reset_model_calls
seqc src/main.seq
expect_status 7
expect_err "another seqc is already building or running this project"
[ $(($(date +%s) - START)) -lt 5 ] || fail "seqc waited for the lock"
expect_model_calls ""
seqc clean
expect_status 7
expect_err "another seqc is building or running this project"
kill "$HOLDER" 2>/dev/null
wait "$HOLDER" 2>/dev/null
seqc src/main.seq
expect_status 0

# --- seqc clean. --------------------------------------------------------------
[ -d output/temp/runs ] || fail "no run records to clean"
seqc clean
expect_status 0
expect_out "[remove] output/temp/runs"
expect_no_file output/temp/runs
# Build artifacts, outputs, inputs, and templates stay.
expect_file output/temp/files.bin
expect_file output/temp/build.json
expect_content output/total.txt 15
expect_content input/data.txt input
expect_file output/temp/.gitignore
# The cached build still works after cleaning run records.
reset_model_calls
seqc src/main.seq
expect_status 0
expect_out "reusing the accepted build"
expect_model_calls ""

seqc clean --all
expect_status 0
for file in files.c files.s files.bin build.json test/files_test.cc \
  test/files_test.bin runs; do
  expect_no_file "output/temp/$file"
done
expect_content output/total.txt 15
expect_content output/numbers.txt "1
2
3
4
5"
expect_content input/data.txt input
expect_file input/.empty
expect_file src/main.seq
expect_file output/temp/.gitignore
expect_content output/temp/Makefile "# my own rules"
[ -d output/temp/test ] || fail "clean --all removed output/temp/test"
# After clean --all the next run builds again.
seqc src/main.seq
expect_status 0
expect_out "[cache]    building: no accepted build yet"

# clean outside a project is refused.
cd "$SEQ_WORK" || exit 1
seqc clean
expect_status 7
expect_err "no Seq project here"
cd files || exit 1

# --- Input rules. -------------------------------------------------------------
# The marker is not an input; real files are listed with their hashes.
mkdir -p input/nested
echo "a,b" >input/nested/table.csv
seqc src/main.seq
expect_status 0
expect_out "[inputs]   2 file(s)"
IM="$(latest_run)input-manifest.json"
expect_json "$IM" '[f["path"] for f in d["files"]]' \
  "['data.txt', 'nested/table.csv']"
expect_json "$IM" 'd["excluded"][0]["path"]' .empty
expect_json "$IM" 'd["files"][0]["sha256"]' \
  "$(sha256sum input/data.txt | cut -d' ' -f1)"
# The model sees the input text, delimited as data.
grep -q "BEGIN DATA" "$(latest_run)attempts/generate-1.request.json" ||
  fail "the input excerpt did not reach the model"

# A symbolic link in input/ is rejected before any inference.
reset_model_calls
ln -s /etc/passwd input/link.txt
seqc src/main.seq
expect_status 7
expect_err "input/link.txt is a symbolic link"
expect_model_calls ""
rm input/link.txt

# So is a file over the size limit.
seqc --set inputs.max_file_mb=0 src/main.seq
expect_status 7
expect_err "inputs.max_file_mb"
expect_model_calls ""

echo "ok"
