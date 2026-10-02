#!/bin/bash
# The llama-server adapter end to end: seqc starts the inference server with
# the locked weights, talks to it over loopback HTTP, and stops it. A local
# program plays llama-server; no model is involved.
. "$SEQ_INTEGRATION/lib.sh"

unset SEQC_MODEL_ADAPTER # the default adapter, llama-server

# --- A locked model with its weights in the cache. ----------------------------
FILE="tiny-q4.gguf"
printf 'these bytes stand in for model weights\n' >"$SEQ_WORK/weights.bin"
SHA=$(sha256sum "$SEQ_WORK/weights.bin" | cut -d' ' -f1)
CACHED="$XDG_CACHE_HOME/seqc/models/$SHA/$FILE"
mkdir -p "$(dirname "$CACHED")"
cp "$SEQ_WORK/weights.bin" "$CACHED"

write_lock() {
  cat >seq.lock <<EOF
{
  "lock_version": 1,
  "declared_url": "https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct",
  "declared_revision": "",
  "repo": "example/tiny-model-GGUF",
  "revision": "rev-one",
  "file": "$FILE",
  "quantization": "q4",
  "sha256": "$SHA",
  "grammar_version": 1,
  "prompt_version": 1
}
EOF
}

# --- A program that stands in for llama-server. -------------------------------
cat >"$SEQ_WORK/llama-server" <<EOF
#!/bin/bash
exec "$PYTHON" "$SEQ_INTEGRATION/mock_llama_server.py" "\$@"
EOF
chmod +x "$SEQ_WORK/llama-server"
export SEQC_MODEL_LLAMA_SERVER="$SEQ_WORK/llama-server"
export MOCK_LLAMA_LOG="$SEQ_WORK/llama-requests.jsonl"

# mock_script NAME ARGS...: like `script`, for the mock server.
mock_script() {
  local name=$1
  shift
  "$PYTHON" "$SEQ_INTEGRATION/make_script.py" "$SEQ_WORK/$name.json" "$@" ||
    fail "cannot build script $name"
  export MOCK_LLAMA_SCRIPT="$SEQ_WORK/$name.json"
}

project files files
write_lock
mock_script good --plan "$W/files/plan.json" \
  --generate "$W/files/steps_syntax.c" "$W/files/steps.c" \
  --test "$W/files/tests.cc"

# --- The whole pipeline through the adapter, including one repair. -----------
seqc src/main.seq
expect_status 0
expect_out "[model]    starting llama-server"
expect_out "[repair]   attempt 2 of 3"
expect_out "sum=15"
expect_out "[test]     2 passed, 0 failed"
expect_content output/total.txt 15
B=output/temp/build.json
expect_json $B 'd["model"]["adapter"]' llama-server
expect_json $B 'd["model"]["lock"]["sha256"]' "$SHA"
expect_json $B 'd["model"]["lock"]["revision"]' rev-one
expect_json $B 'd["generation"]["seed"]' 1

# What the server was asked: plan, generate, repair, test, with sampling
# pinned and the response constrained to a schema.
REQ="$MOCK_LLAMA_LOG"
[ "$(wc -l <"$REQ")" -eq 4 ] || fail "expected 4 completion requests"
line() { sed -n "$1p" "$REQ" >"$SEQ_WORK/request.json"; }
line 1
R="$SEQ_WORK/request.json"
expect_json "$R" 'd["temperature"]' 0.0
expect_json "$R" 'd["seed"]' 1
expect_json "$R" 'd["max_tokens"]' 4096
expect_json "$R" 'd["stream"]' False
expect_json "$R" 'd["response_format"]["type"]' json_schema
expect_json "$R" 'd["response_format"]["json_schema"]["name"]' seq_plan
expect_json "$R" 'd["response_format"]["json_schema"]["schema"]["properties"]["steps"]["minItems"]' 2
expect_json "$R" '[m["role"] for m in d["messages"]]' "['system', 'user']"
expect_json "$R" '"read numbers.txt and write their sum" in d["messages"][1]["content"]' True
line 3
expect_json "$R" 'd["response_format"]["json_schema"]["name"]' seq_repair
expect_json "$R" '"undeclared_variable" in d["messages"][1]["content"]' True
line 4
expect_json "$R" 'd["response_format"]["json_schema"]["name"]' seq_test

# The server was stopped when the build finished, and its log is kept.
sleep 0.3
if pgrep -f "mock_llama_server.py --model $CACHED" >/dev/null; then
  fail "the inference server is still running"
fi
expect_file "$(latest_run)logs/llama-server.log"

# A cached run does not start the server at all.
: >"$REQ"
seqc src/main.seq
expect_status 0
expect_out "reusing the accepted build"
expect_no_out "[model]"
[ ! -s "$REQ" ] || fail "a cached run contacted the model"

# --- A response cut off at the output budget is a failure, not a program. ----
printf '{"plan": [], "generate": [{"$length": true}], "test": []}' \
  >"$SEQ_WORK/length.json"
"$PYTHON" - "$W/files/plan.json" "$SEQ_WORK/length.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[2]))
d["plan"].append(json.load(open(sys.argv[1])))
json.dump(d, open(sys.argv[2], "w"))
EOF
export MOCK_LLAMA_SCRIPT="$SEQ_WORK/length.json"
seqc --rebuild src/main.seq
expect_status 4
expect_err "the response was cut off at the output budget of 4096 tokens"

# --- An HTTP error from the server. -------------------------------------------
mock_script empty --plan "$W/files/plan.json"
seqc --rebuild src/main.seq
expect_status 4
expect_err "inference server answered HTTP 500"

# --- Weights that do not match the lock are never loaded. ---------------------
mock_script good2 --plan "$W/files/plan.json" --generate "$W/files/steps.c" \
  --test "$W/files/tests.cc"
echo "tampered" >>"$CACHED"
: >"$REQ"
seqc --rebuild src/main.seq
expect_status 4
expect_err "model weights do not match seq.lock"
[ ! -s "$REQ" ] || fail "tampered weights were served"
cp "$SEQ_WORK/weights.bin" "$CACHED"

# --- The server fails to start. -----------------------------------------------
MOCK_LLAMA_FAIL=1 "$SEQC" --rebuild src/main.seq >"$OUT" 2>"$ERR"
STATUS=$?
expect_status 4
expect_err "the inference server exited during startup; see"
grep -q "failed to load model" "$(latest_run)logs/llama-server.log" ||
  fail "the server log was not kept"

# --- No inference runtime installed. ------------------------------------------
seqc --set model.llama_server=no-such-llama-server --rebuild src/main.seq
expect_status 4
expect_err "inference runtime 'no-such-llama-server' was not found"

# --- An already running server, selected with model.endpoint. -----------------
PORT=$("$PYTHON" -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
"$SEQ_WORK/llama-server" --model "$CACHED" --host 127.0.0.1 --port "$PORT" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
for _ in $(seq 100); do
  curl -fs "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break
  sleep 0.05
done
seqc --set model.endpoint="http://127.0.0.1:$PORT" --rebuild src/main.seq
expect_status 0
expect_out "sum=15"
kill -0 $SERVER 2>/dev/null || fail "seqc stopped a server it did not start"

# The running server must be serving the locked weights.
sed -i "s/\"file\": \"$FILE\"/\"file\": \"other.gguf\"/" seq.lock
seqc --set model.endpoint="http://127.0.0.1:$PORT" --rebuild src/main.seq
expect_status 4
expect_err "serves '$FILE', not the locked other.gguf"
write_lock

# Remote endpoints are not supported in v0.1: nothing leaves the machine.
seqc --set model.endpoint="http://10.0.0.45:8080" --rebuild src/main.seq
expect_status 4
expect_err "model.endpoint must be a loopback address in v0.1"
seqc --set model.endpoint="https://example.com" --rebuild src/main.seq
expect_status 4
expect_err "model.endpoint must look like http://127.0.0.1:8080"

echo "ok"
