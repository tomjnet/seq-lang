#!/usr/bin/env python3
"""A local stand-in for the model hub, for testing `seqc model pull`.

Usage: mock_hub.py PORT_FILE LOG_FILE REPO FILE REVISION WEIGHTS_PATH

Serves, for the one repository REPO (owner/name):
  GET /api/models/REPO                  -> {"sha": REVISION}
  GET /api/models/REPO/tree/REVISION    -> [{"path": FILE, "lfs": {"oid": <sha256>}}]
  GET /REPO/resolve/REVISION/FILE       -> the bytes of WEIGHTS_PATH

REVISION and the weights are read again on every request, so a test can
change them while the server runs. The chosen port is written to PORT_FILE
and every request path is appended to LOG_FILE.
"""

import hashlib
import http.server
import json
import sys

port_file, log_file, repo, file_name, revision_file, weights_path = sys.argv[1:7]


def revision() -> str:
    with open(revision_file, "r", encoding="utf-8") as f:
        return f.read().strip()


def weights() -> bytes:
    with open(weights_path, "rb") as f:
        return f.read()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status: int, body: bytes, content_type: str):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        with open(log_file, "a", encoding="utf-8") as log:
            log.write(self.path + "\n")
        rev = revision()
        if self.path == f"/api/models/{repo}":
            body = json.dumps({"id": repo, "sha": rev}).encode()
            self.reply(200, body, "application/json")
        elif self.path == f"/api/models/{repo}/tree/{rev}":
            listing = [
                {"type": "file", "path": "README.md", "size": 10},
                {
                    "type": "file",
                    "path": file_name,
                    "size": len(weights()),
                    "lfs": {"oid": hashlib.sha256(weights()).hexdigest()},
                },
            ]
            self.reply(200, json.dumps(listing).encode(), "application/json")
        elif self.path == f"/{repo}/resolve/{rev}/{file_name}":
            self.reply(200, weights(), "application/octet-stream")
        else:
            self.reply(404, b"not found\n", "text/plain")


server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
with open(port_file, "w", encoding="utf-8") as f:
    f.write(str(server.server_address[1]))
server.serve_forever()
