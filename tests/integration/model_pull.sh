#!/bin/bash
# `seqc model pull` resolves, downloads, verifies, and locks the model. It is
# the only command that downloads anything. A local server plays the hub.
. "$SEQ_INTEGRATION/lib.sh"

unset SEQC_MODEL_ADAPTER

REPO="example/tiny-model-GGUF"
FILE="tiny-q4.gguf"
WEIGHTS="$SEQ_WORK/hub-weights.bin"
REVISION_FILE="$SEQ_WORK/hub-revision.txt"
HUB_LOG="$SEQ_WORK/hub-requests.log"
printf 'these bytes stand in for model weights\n' >"$WEIGHTS"
echo "rev-one" >"$REVISION_FILE"
SHA=$(sha256sum "$WEIGHTS" | cut -d' ' -f1)

"$PYTHON" "$SEQ_INTEGRATION/mock_hub.py" "$SEQ_WORK/hub-port" "$HUB_LOG" \
  "$REPO" "$FILE" "$REVISION_FILE" "$WEIGHTS" &
HUB=$!
trap 'kill $HUB 2>/dev/null' EXIT
for _ in $(seq 100); do
  [ -s "$SEQ_WORK/hub-port" ] && break
  sleep 0.05
done
[ -s "$SEQ_WORK/hub-port" ] || fail "the mock hub did not start"
export SEQC_MODEL_HUB_URL="http://127.0.0.1:$(cat "$SEQ_WORK/hub-port")"

downloads() {
  grep -c "/resolve/" "$HUB_LOG" 2>/dev/null || true
}

# --- Outside a project. -------------------------------------------------------
seqc model pull
expect_status 7
expect_err "no Seq project here"

project demo hello

# --- The reference model is pinned: other bytes are refused. ------------------
# The hub here does not serve the pinned artifact at all, so the download
# fails and nothing is locked. seqc never substitutes another model.
seqc model pull
expect_status 4
expect_out "Artifact:     Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF / qwen2.5-coder-1.5b-instruct-q4_k_m.gguf"
expect_out "SHA-256:      cc324af070c2ecbfd324a30884d2f951a7ff756aba85cb811a6ec436933bb046"
expect_err "download failed"
expect_no_file seq.lock

# --- An unknown model without a mapping is an error. --------------------------
sed -i 's|Qwen/Qwen2.5-Coder-1.5B-Instruct|example/tiny-model|' src/main.seq
seqc model pull
expect_status 4
expect_err "no runtime artifact is known for https://huggingface.co/example/tiny-model"
expect_no_file seq.lock

# --- A user mapping with the wrong hash: verified, deleted, not locked. ------
export SEQC_MODEL_GGUF_REPO="$REPO" SEQC_MODEL_GGUF_FILE="$FILE"
export SEQC_MODEL_GGUF_REVISION="rev-one" SEQC_MODEL_GGUF_QUANTIZATION="q4"
export SEQC_MODEL_GGUF_SHA256
SEQC_MODEL_GGUF_SHA256=$(printf '0%.0s' $(seq 64))
seqc model pull
expect_status 4
expect_err "the downloaded file does not match the pinned SHA-256"
expect_err "Nothing was locked"
expect_no_file seq.lock
[ -z "$(find "$XDG_CACHE_HOME" -type f)" ] ||
  fail "a rejected download was left in the cache"

# --- A correct mapping: downloaded, verified, cached, and locked. -------------
SEQC_MODEL_GGUF_SHA256=$SHA
: >"$HUB_LOG"
seqc model pull
expect_status 0
expect_out "[download] $SEQC_MODEL_HUB_URL/$REPO/resolve/rev-one/$FILE"
expect_out "nothing from the project is sent"
expect_out "Model is ready."
CACHED="$XDG_CACHE_HOME/seqc/models/$SHA/$FILE"
expect_file "$CACHED"
cmp -s "$CACHED" "$WEIGHTS" || fail "the cached weights differ"
[ "$(downloads)" -eq 1 ] || fail "expected one download"
expect_json seq.lock 'd["declared_url"]' https://huggingface.co/example/tiny-model
expect_json seq.lock 'd["repo"]' "$REPO"
expect_json seq.lock 'd["revision"]' rev-one
expect_json seq.lock 'd["file"]' "$FILE"
expect_json seq.lock 'd["quantization"]' q4
expect_json seq.lock 'd["sha256"]' "$SHA"
expect_json seq.lock 'd["grammar_version"]' 1
expect_json seq.lock 'd["prompt_version"]' 1

# --- A second pull verifies the cache and downloads nothing. ------------------
unset SEQC_MODEL_GGUF_REPO SEQC_MODEL_GGUF_FILE SEQC_MODEL_GGUF_REVISION
unset SEQC_MODEL_GGUF_SHA256 SEQC_MODEL_GGUF_QUANTIZATION
seqc model pull # Later pulls use the locked values, not the configuration.
expect_status 0
expect_out "[verify]"
expect_no_out "[download]"
[ "$(downloads)" -eq 1 ] || fail "the second pull downloaded again"

# --- Tampered weights are detected and replaced. ------------------------------
echo "tampered" >>"$CACHED"
seqc model pull
expect_status 0
expect_out "cached file does not match; downloading again"
cmp -s "$CACHED" "$WEIGHTS" || fail "tampered weights were not replaced"
[ "$(downloads)" -eq 2 ] || fail "expected a second download"

# doctor sees the locked model.
seqc doctor
expect_out "[ ok ] model: $REPO / $FILE at rev-one"

# --- --update re-resolves the revision and hash, and rewrites the lock. ------
printf 'newer weights\n' >"$WEIGHTS"
echo "rev-two" >"$REVISION_FILE"
NEW_SHA=$(sha256sum "$WEIGHTS" | cut -d' ' -f1)
seqc model pull # Without --update the lock still pins rev-one.
expect_status 0
expect_json seq.lock 'd["revision"]' rev-one
seqc model pull --update
expect_status 0
expect_out "[resolve]"
expect_json seq.lock 'd["revision"]' rev-two
expect_json seq.lock 'd["sha256"]' "$NEW_SHA"
expect_file "$XDG_CACHE_HOME/seqc/models/$NEW_SHA/$FILE"

# --- A lock for another model needs --update. ---------------------------------
sed -i 's|example/tiny-model|example/another-model|' src/main.seq
seqc model pull
expect_status 4
expect_err "seq.lock was written for https://huggingface.co/example/tiny-model"
expect_json seq.lock 'd["revision"]' rev-two

# --- An unreachable hub is a model failure. -----------------------------------
sed -i 's|example/another-model|example/tiny-model|' src/main.seq
kill $HUB
wait $HUB 2>/dev/null
rm -rf "$XDG_CACHE_HOME"
seqc model pull
expect_status 4
expect_err "download failed"

# --- A compile never downloads. -----------------------------------------------
script hello --plan "$W/hello/plan.json" --generate "$W/hello/steps.c" \
  --test "$W/hello/tests.cc"
seqc src/main.seq # adapter: llama-server (the default)
expect_status 4
expect_err "model weights are not downloaded"
expect_err "run \`seqc model pull\`"
[ -z "$(find "$XDG_CACHE_HOME" -type f 2>/dev/null)" ] ||
  fail "a compile downloaded something"

echo "ok"
