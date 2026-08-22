#!/usr/bin/env python3
"""Mock OpenAI-compatible LLM server for the desktop mobile E2E test.

Serves:
  POST /v1/chat/completions  - returns one `exec` tool call on the first
                               request (no tool result in history), then a
                               final answer. Streams SSE like the real API.
  POST /v1/embeddings        - returns a deterministic 1536-dim vector.

The command issued by the tool call is chosen per-request, in this order:
  1. contents of CMD_FILE (default C:/tmp/mc_mock_cmd.txt) — lets a test
     harness switch scenarios without restarting the server;
  2. MOCK_CMD environment variable;
  3. "ls | sort".

Usage:  python mock_llm.py [port]      (default port 9876)
"""
import json
import os
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9876
CMD_FILE = os.environ.get("MOCK_CMD_FILE", "C:/tmp/mc_mock_cmd.txt")
EMBED_DIM = 1536


def get_cmd():
    try:
        with open(CMD_FILE, "r") as f:
            cmd = f.read().strip()
        if cmd:
            return cmd
    except FileNotFoundError:
        pass
    return os.environ.get("MOCK_CMD", "ls | sort")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode(errors="replace")
        if self.path.endswith("/embeddings"):
            vec = [0.01 * ((i % 7) + 1) for i in range(EMBED_DIM)]
            self._send(200, json.dumps({"data": [{"embedding": vec}]}))
            return
        if self.path.endswith("/chat/completions"):
            req = json.loads(body)
            msgs = req.get("messages", [])
            has_tool_result = any(m.get("role") == "tool" for m in msgs)
            sse = []
            if not has_tool_result:
                args = json.dumps({"command": get_cmd()})
                chunk = {"choices": [{"delta": {"tool_calls": [
                    {"index": 0, "id": "call_mock_1",
                     "function": {"name": "exec", "arguments": args}}]}}]}
                sse.append("data: " + json.dumps(chunk) + "\n\n")
            else:
                chunk = {"choices": [{"delta": {
                    "content": "MOCK_FINAL_ANSWER: task completed"}}]}
                sse.append("data: " + json.dumps(chunk) + "\n\n")
            sse.append("data: [DONE]\n\n")
            self._send(200, "".join(sse), "text/event-stream")
            return
        self._send(404, json.dumps({"error": "not found"}))


if __name__ == "__main__":
    print(f"mock LLM listening on 127.0.0.1:{PORT} (cmd file: {CMD_FILE})",
          flush=True)
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
