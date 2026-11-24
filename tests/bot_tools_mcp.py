import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer


TOOL_RESULTS = {
    "numeric-content": {"content": 42, "isError": False},
    "missing-content": {"structuredContent": {}},
    "invalid-block": {"content": [{"type": "text", "text": 42}]},
    "invalid-resource": {"content": [{"type": "resource", "resource": {"uri": "fixture://data"}}]},
    "invalid-resource-mime-type": {
        "content": [{"type": "resource", "resource": {"uri": "fixture://data", "text": "Ready", "mimeType": 42}}],
    },
    "invalid-annotations": {"content": [{"type": "text", "text": "Ready", "annotations": False}]},
    "invalid-structured-content": {"content": [], "structuredContent": []},
    "invalid-is-error": {"content": [], "isError": 1},
    "extension": {
        "content": [
            {"type": "text", "text": "Tool evidence", "annotations": {"audience": ["assistant"]}},
            {"type": "fixture-extension", "payload": 7, "annotations": False},
        ],
        "structuredContent": {"count": 1},
    },
    "compatible-content": {
        "content": [
            {"type": "resource", "resource": {"uri": "fixture://text", "text": "Ready", "blob": None}},
            {"type": "resource", "resource": {"uri": "fixture://blob", "blob": "YQ==", "text": None}},
            {"type": "text", "text": "Legacy", "annotations": {"lastModified": 1700000000}},
        ],
    },
    "tool-failure": {"content": [{"type": "text", "text": "Tool failed"}], "isError": True},
}


class Fixture(BaseHTTPRequestHandler):
    calls = []

    def send_json(self, status, data=None, session=None, stream=False):
        payload = json.dumps(data).encode() if data is not None else b""
        content_type = "application/json"
        if stream:
            content_type = "text/event-stream; charset=utf-8"
            unrelated = {"jsonrpc": "2.0", "id": data["id"] + 1, "result": {}}
            frames = [f": keepalive\r\ndata: {json.dumps(unrelated)}\r\n\r\n"]
            frames.extend(f"data: {line}\r\n" for line in json.dumps(data, indent=2).splitlines())
            payload = ("".join(frames) + "\r\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        if session:
            self.send_header("Mcp-Session-Id", session)
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        method = request["method"]
        params = request.get("params", {})
        cursor = params.get("cursor")
        name = params.get("name")
        session = self.headers.get("Mcp-Session-Id")
        self.calls.append((method, session, name if method == "tools/call" else cursor))
        if method == "notifications/initialized":
            self.send_json(503 if self.path.endswith("/failed-init") else 202)
            return
        result = {"jsonrpc": "2.0", "id": request["id"]}
        if method == "initialize":
            result["result"] = {"protocolVersion": "2025-03-26", "capabilities": {"tools": {}, "resources": {}}}
            self.send_json(200, result, f"session-{len(self.calls)}")
        elif method == "tools/list" and session == "session-1" and cursor in (None, "session-bound"):
            if cursor:
                result["result"] = {"tools": [
                    {"name": "tool-failure", "annotations": {"readOnlyHint": False}},
                    {"name": "untyped-hint", "annotations": {"readOnlyHint": "true"}},
                ]}
            else:
                result["result"] = {"tools": [
                    {"name": "extension", "annotations": {"readOnlyHint": True}},
                ], "nextCursor": "session-bound"}
            self.send_json(200, result, stream=cursor is None)
        elif method == "tools/call" and name in TOOL_RESULTS:
            result["result"] = TOOL_RESULTS[name]
            self.send_json(200, result)
        else:
            result["error"] = {"code": -32602, "message": "Invalid session or cursor"}
            self.send_json(400, result)

    def do_DELETE(self):
        self.calls.append(("delete", self.headers.get("Mcp-Session-Id"), None))
        self.send_json(405 if self.path.endswith("/failed-init") else 204)


with HTTPServer(("127.0.0.1", 0), Fixture) as server:
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        subprocess.run(
            [sys.argv[1], f"http://127.0.0.1:{server.server_port}/mcp"],
            env={**os.environ, "FLOCK_MCP_TEST_TOKEN": "isError"},
            check=True,
            timeout=20,
        )
    finally:
        server.shutdown()
        worker.join()
    expected = [
        ("initialize", None, None),
        ("notifications/initialized", "session-1", None),
        ("tools/list", "session-1", None),
        ("tools/list", "session-1", "session-bound"),
        ("delete", "session-1", None),
    ]
    for name in (*TOOL_RESULTS, "rejected", None):
        repetitions = 2 if name == "extension" else 1
        for _ in range(repetitions):
            session = f"session-{len(expected) + 1}"
            expected.extend([("initialize", None, None), ("notifications/initialized", session, None)])
            if name is not None:
                expected.append(("tools/call", session, name))
            expected.append(("delete", session, None))
    assert Fixture.calls == expected, Fixture.calls
