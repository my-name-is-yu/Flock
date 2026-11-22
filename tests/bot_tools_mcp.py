import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer


class Fixture(BaseHTTPRequestHandler):
    calls = []

    def send_json(self, status, data=None, session=None):
        payload = json.dumps(data).encode() if data is not None else b""
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        if session:
            self.send_header("Mcp-Session-Id", session)
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        method = request["method"]
        cursor = request.get("params", {}).get("cursor")
        session = self.headers.get("Mcp-Session-Id")
        self.calls.append((method, session, cursor))
        if method == "notifications/initialized":
            self.send_json(202)
            return
        result = {"jsonrpc": "2.0", "id": request["id"]}
        if method == "initialize":
            result["result"] = {"protocolVersion": "2025-03-26", "capabilities": {"tools": {}, "resources": {}}}
            self.send_json(200, result, f"session-{len(self.calls)}")
        elif method == "tools/list" and session == "session-1" and cursor in (None, "session-bound"):
            result["result"] = {"tools": ["second"]} if cursor else {"tools": ["first"], "nextCursor": "session-bound"}
            self.send_json(200, result)
        else:
            result["error"] = {"code": -32602, "message": "Invalid session or cursor"}
            self.send_json(400, result)

    def do_DELETE(self):
        self.calls.append(("delete", self.headers.get("Mcp-Session-Id"), None))
        self.send_json(204)


with HTTPServer(("127.0.0.1", 0), Fixture) as server:
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}/mcp"], check=True, timeout=20)
    finally:
        server.shutdown()
        worker.join()
    assert Fixture.calls == [
        ("initialize", None, None),
        ("notifications/initialized", "session-1", None),
        ("tools/list", "session-1", None),
        ("tools/list", "session-1", "session-bound"),
        ("delete", "session-1", None),
    ], Fixture.calls
