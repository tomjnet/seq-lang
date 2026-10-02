#!/usr/bin/env python3
"""A local stand-in for llama.cpp's llama-server, for testing seqc's adapter.

It accepts the command line seqc uses (--model, --host, --port, --ctx-size,
--parallel) and implements the endpoints the adapter calls:

  GET  /health               -> 200 once "loaded"
  GET  /props                -> {"model_path": <--model>}
  POST /tokenize             -> {"tokens": [...]}, one token per 4 bytes
  POST /v1/chat/completions  -> the next scripted response for the request

Responses come from the fake-adapter script named by MOCK_LLAMA_SCRIPT (see
make_script.py); the request family is taken from the name of the JSON schema
the adapter sends. Every completion request body is appended to
MOCK_LLAMA_LOG as one JSON line. An entry {"$length": true} answers with
finish_reason "length". MOCK_LLAMA_FAIL=1 makes the server exit at startup.
"""

import http.server
import json
import os
import sys

args = sys.argv[1:]
options = dict(zip(args[0::2], args[1::2]))
model_path = options.get("--model", "")
host = options.get("--host", "127.0.0.1")
port = int(options.get("--port", "0"))

if os.environ.get("MOCK_LLAMA_FAIL") == "1":
    print("mock llama-server: failed to load model", file=sys.stderr)
    sys.exit(1)
if not os.path.isfile(model_path):
    print(f"mock llama-server: no model at {model_path}", file=sys.stderr)
    sys.exit(1)

with open(os.environ["MOCK_LLAMA_SCRIPT"], "r", encoding="utf-8") as f:
    script = json.load(f)
log_path = os.environ.get("MOCK_LLAMA_LOG", "")
counters = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status: int, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, {"status": "ok"})
        elif self.path == "/props":
            self.reply(200, {"model_path": model_path})
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        if self.path == "/tokenize":
            count = (len(request["content"].encode()) + 3) // 4
            self.reply(200, {"tokens": list(range(count))})
            return
        if self.path != "/v1/chat/completions":
            self.reply(404, {"error": "not found"})
            return
        if log_path:
            with open(log_path, "a", encoding="utf-8") as log:
                log.write(json.dumps(request) + "\n")

        name = request["response_format"]["json_schema"]["name"]
        family = {
            "seq_plan": "plan",
            "seq_generate": "generate",
            "seq_repair": "generate",
            "seq_test": "test",
            "seq_test-repair": "test",
        }[name]
        index = counters.get(family, 0)
        counters[family] = index + 1
        entries = script.get(family, [])
        if index >= len(entries):
            self.reply(500, {"error": f"no scripted response for {family}"})
            return
        entry = entries[index]
        finish = "stop"
        if isinstance(entry, dict) and entry.get("$length"):
            finish = "length"
            content = '{"step_functions": "int seq_step_1(seq_ctx *ctx) {'
        elif isinstance(entry, str):
            content = entry
        else:
            content = json.dumps(entry)
        self.reply(
            200,
            {
                "choices": [
                    {
                        "index": 0,
                        "finish_reason": finish,
                        "message": {"role": "assistant", "content": content},
                    }
                ]
            },
        )


http.server.HTTPServer((host, port), Handler).serve_forever()
