"""A loopback-only v0.1.11 wire fixture for the C++ adapter tests."""
import base64
import hashlib
import json
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value=None, code=200, **headers):
        body = json.dumps(value).encode() if value is not None else b""
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for key, item in headers.items():
            self.send_header(key.replace("_", "-"), item)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        origin = self.server.origin
        if self.path == "/fixture/effects":
            self.reply(self.server.state.get("effects", {}))
        elif self.path == "/.well-known/oauth-protected-resource/mcp":
            self.reply({"resource": origin + "/mcp", "authorization_servers": [origin]})
        elif self.path == "/.well-known/oauth-authorization-server":
            self.reply({"issuer": origin, "authorization_endpoint": origin + "/authorize",
                        "registration_endpoint": origin + "/register", "token_endpoint": origin + "/token",
                        "code_challenge_methods_supported": ["S256"], "grant_types_supported": ["authorization_code"],
                        "token_endpoint_auth_methods_supported": ["none"],
                        "scopes_supported": ["orchestration:read", "orchestration:operate"]})
        else:
            self.reply(code=404)

    def do_DELETE(self):
        assert self.headers["Mcp-Session-Id"] == "fixture-session"
        self.reply(code=204)

    def do_POST(self):
        origin = self.server.origin
        state = self.server.state
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/token":
            args = urllib.parse.parse_qs(raw.decode())
            verify = base64.urlsafe_b64encode(hashlib.sha256(args["code_verifier"][0].encode()).digest())
            assert verify.decode().rstrip("=") == state["challenge"]
            assert args["resource"] == [origin + "/mcp"] and args["grant_type"] == ["authorization_code"]
            assert args["code"] == ["fixture-code"] and args["redirect_uri"] == state["registered"]
            state["revoked"] = False
            self.reply({"access_token": "fixture.signed-token", "token_type": "Bearer", "expires_in": 2592000,
                        "scope": state.get("scope", "orchestration:read orchestration:operate")})
            return
        args = json.loads(raw)
        if self.path == "/fixture":
            state.update(args)
            if "authorization" in args:
                auth = urllib.parse.parse_qs(urllib.parse.urlsplit(args["authorization"]).query)
                assert auth["client_id"] == ["fixture-client"] and auth["code_challenge_method"] == ["S256"]
                assert auth["response_type"] == ["code"] and auth["redirect_uri"] == state["registered"]
                assert auth["scope"] == ["orchestration:read orchestration:operate"]
                state["challenge"] = auth["code_challenge"][0]

                def callback():
                    redirect = auth["redirect_uri"][0]
                    wrong = urllib.parse.urlencode({"state": "invalid", "code": "fixture-code", "iss": origin})
                    try:
                        urllib.request.urlopen(redirect + "?" + wrong, timeout=5)
                    except urllib.error.HTTPError as error:
                        assert error.code == 400
                    good = urllib.parse.urlencode({"state": auth["state"][0], "code": "fixture-code", "iss": origin})
                    with urllib.request.urlopen(redirect + "?" + good, timeout=5) as reply:
                        assert reply.status == 200

                threading.Thread(target=callback, daemon=True).start()
            self.reply({})
        elif self.path == "/register":
            assert args["token_endpoint_auth_method"] == "none" and args["grant_types"] == ["authorization_code"]
            state["registered"] = args["redirect_uris"]
            self.reply({"client_id": "fixture-client"}, code=201)
        elif self.path == "/mcp":
            if self.headers.get("Authorization") != "Bearer fixture.signed-token" or state.get("revoked"):
                self.reply(code=401, WWW_Authenticate='Bearer resource_metadata="' + origin +
                           '/.well-known/oauth-protected-resource/mcp"')
                return
            envelope = {"jsonrpc": "2.0", "id": args.get("id")}
            if args["method"] == "initialize":
                assert args["params"]["protocolVersion"] == "2025-06-18"
                envelope["result"] = {"protocolVersion": "2025-06-18"}
                self.reply(envelope, Mcp_Session_Id="fixture-session")
                return
            assert self.headers["Mcp-Session-Id"] == "fixture-session"
            assert self.headers["MCP-Protocol-Version"] == "2025-06-18"
            if args["method"] == "notifications/initialized":
                self.reply(code=202)
                return
            assert args["method"] == "tools/call"
            tool = args["params"]
            content = self.tool(tool["name"], tool["arguments"])
            envelope["result"] = {"content": [{"type": "text", "text": json.dumps(content)}],
                                  "isError": "code" in content}
            if self.server.peer_recovery and tool["name"] == "t3_thread_launch":
                self.server.launch_accepted.set()
                assert self.server.allow_launch_disconnect.wait(15), "crash harness did not kill the Runtime"
                self.close_connection = True
                return
            self.reply(envelope)
        else:
            self.reply(code=404)

    def tool(self, name, args):
        state = self.server.state
        scenario = state.get("scenario", "settled")
        questions = [{"id": "color", "header": "Colors", "question": "Which colors?", "multiSelect": True,
                      "allowCustomAnswer": False, "options": [{"label": "Green", "value": "green", "description": ""},
                                                              {"label": "blue", "description": ""}]}]
        if (name in ("t3_thread_launch", "t3_thread_send", "t3_thread_interrupt") or
                (name == "t3_pending_request" and args["action"] == "respond")):
            state.setdefault("effects", {}).setdefault(name, []).append(args)
        assert "environmentId" not in args
        if name == "t3_environment_read":
            return {"environmentId": "env-1", "serverVersion": "0.1.11"}
        if name == "t3_project_read":
            assert args["projectId"] == "project"
            return {"id": "project", "title": "Release Pipeline", "defaultModelSelection": None}
        if name == "t3_thread_launch":
            assert args["projectId"] == "project"
            assert "modelSelection" not in args and "workspaceStrategy" not in args
            if self.server.peer_recovery:
                state["launches"].append(args["clientRequestId"])
                assert len(state["launches"]) == 1, "Runtime sent a second launch"
                assert "Verify the fixture" in args["message"]
            else:
                assert args["clientRequestId"] == "flock-action-0123"
                assert args["message"].startswith("[Flock action flock-action-0123]\n\n  preserve")
            state["launched"] = args["message"]
            return {"projectId": "project", "threadId": "thread", "link": "t3://thread",
                    "runId": None if state.get("delayRunId") else "root"}
        if name == "t3_thread_send" and args["mode"] == "steer":
            assert scenario in ("running", "resumed") and args["threadId"] == "thread"
            assert args["clientRequestId"] == "flock-turn-7-2"
            return {"threadId": "thread", "runId": "continued" if scenario == "resumed" else "root",
                    "delivery": "steered"}
        if name == "t3_thread_send":
            assert scenario == "settled" and args["mode"] == "queue" and args["threadId"] == "thread"
            assert args["clientRequestId"] == "flock-action-01234"
            state["followup"] = args["message"]
            return {"threadId": "thread", "runId": "follow", "delivery": "started", "link": "t3://thread"}
        if name == "t3_queue":
            assert args["action"] == "list" and args["threadId"] == "thread"
            return {"items": [], "nextCursor": None}
        if name == "t3_thread_list":
            assert args["projectId"] == "project"
            ids = ["thread", "duplicate"] if state.get("duplicate") else ["thread"]
            start = args.get("cursor", 0)
            end = min(start + args.get("limit", len(ids)), len(ids))
            return {"projectId": "project", "threads": [self.summary(t) for t in ids[start:end]],
                    "total": len(ids), "nextCursor": end if end < len(ids) else None}
        if name == "t3_thread_wait":
            assert args["threadId"] == "thread" and args["runId"] == "root" and args["timeoutMs"] <= 400
            if scenario == "resumed":
                return {"threadId": "thread", "runId": "continued", "timedOut": True, "status": "running"}
            active = scenario in ("running", "question", "ambiguous", "approval", "race")
            timed_out = active or scenario in ("settling", "manual-active")
            reply = {"threadId": "thread", "runId": "root", "timedOut": timed_out,
                     "status": "running" if scenario == "running" else "waiting" if active else "completed"}
            if not timed_out and args["untilDelegatedWorkSettled"]:
                reply["resultRun"] = {"runId": "later" if scenario.endswith("-result") else "root", "status": "completed"}
            return reply
        if name == "t3_thread_interrupt":
            assert scenario == "running" and args == {"threadId": "thread", "runId": "root"}
            return {"threadId": "thread", "runId": "root", "status": "interrupt_requested"}
        if name == "t3_pending_request":
            action = args["action"]
            if action == "list":
                return {"requestIds": ["request"] if scenario in ("question", "ambiguous", "race") else []}
            assert args["requestId"] == "request" and scenario in ("question", "ambiguous", "race")
            if action == "read":
                return {"requestId": "request", "questions": questions}
            assert action == "respond" and scenario != "ambiguous" and args["answers"] == {"color": ["green", "blue"]}
            state["scenario"] = "running"
            if scenario == "race":
                return {"_tag": "OrchestratorMcpFailure", "code": "invalid_request",
                        "message": "The user-input request was already answered."}
            return {"sequence": 10}
        assert name == "t3_thread_read", name
        thread_id = args["threadId"]
        active = {"manual-active": "unrelated", "running": "root", "resumed": "continued"}.get(scenario)
        latest = "later" if scenario.endswith("-result") else (active or "root")
        count = 1 if scenario in ("question", "ambiguous", "approval", "race") else 0
        summary = self.summary(thread_id) | {"activeRunId": active, "latestRunId": latest, "archived": False,
                                             "pendingRequestCount": count}
        recent = [{"runId": "root", "ordinal": 1, "status": "completed"}]
        if scenario.endswith("-result"):
            recent.insert(0, {"runId": "later", "ordinal": 2, "status": "completed"})
        rows = [self.item(thread_id, 0, "root", "user_message", state["launched"], source="mcp"),
                self.item(thread_id, 1, "root", "assistant_message", "root report")]
        if scenario == "delegated-result":
            rows.extend([self.item(thread_id, 2, "root", "subagent", "done") | {"subagentId": "child-task"},
                         self.item(thread_id, 3, "later", "user_message", "Delegated task child-task [completed]",
                                   source="server"),
                         self.item(thread_id, 4, "later", "assistant_message", "delegated report")])
        elif scenario in ("notification-result", "mixed-result", "shortened-result"):
            label = "Owned delegate"
            summary_label = label
            if scenario == "shortened-result":
                prefix = "Read notes.md and identify which action items are assigned to "
                label = prefix + "the..."
                summary_label = prefix + "the release owner…"
            rows.extend([self.item(thread_id, 2, "root", "subagent", "done") |
                         {"subagentId": "child-task", "title": label},
                         self.item(thread_id, 3, "later", "notification", f'Delegated task "{summary_label}" finished') |
                         {"creationSource": None, "createdBy": None}])
            if scenario == "mixed-result":
                rows.append(self.item(thread_id, 4, "later", "user_message", "simultaneous manual input", source="web"))
            rows.append(self.item(thread_id, 5, "later", "assistant_message", "delegated report"))
        elif scenario == "manual-result":
            rows.extend([self.item(thread_id, 2, "later", "user_message", "manual request", source="web"),
                         self.item(thread_id, 3, "later", "assistant_message", "manual result")])
        if scenario in ("question", "ambiguous", "race"):
            rows.append(self.item(thread_id, 5, "root", "user_input_request", json.dumps(questions), status="waiting"))
        if scenario == "ambiguous":
            rows.append(self.item(thread_id, 6, "later", "user_input_request", json.dumps(questions)))
        if "followup" in state:
            rows.append(self.item(thread_id, 7, "follow", "user_message", state["followup"], source="mcp"))
        if scenario == "session-read":
            if thread_id != "manual":
                return {"code": "thread_not_found", "message": "The thread was not found."}
            if state.get("denyRead"):
                return {"code": "capability_denied", "message": "Read scope required."}
            summary |= {"title": "Staging investigation", "status": "failed", "runCount": 4, "archived": True,
                        "createdAt": "2026-11-21T01:00:00.000Z", "updatedAt": "2026-11-21T13:00:00.000Z"}
            rows = [self.item(thread_id, 0, "root", "user_message", "Investigate staging", source="web"),
                    self.item(thread_id, 1, "root", "command_execution", "$ ctest --output-on-failure\nTest 7 failed",
                              status="failed"),
                    self.item(thread_id, 2, "root", "error", "staging health check failed", status="failed"),
                    self.item(thread_id, 3, "root", "assistant_message", "I found the failure"),
                    self.item(thread_id, 4, "follow", "user_message", "Confirm the timeout", source="web"),
                    self.item(thread_id, 5, "follow", "assistant_message", "staging health check failed"),
                    self.item("unrelated", 6, "other", "assistant_message", "private history") | {"visibility": "inherited"},
                    self.item(thread_id, 7, "follow", "assistant_message", "界" * 8000)]
            if state.get("longHistory"):
                rows.extend(self.item(thread_id, i, "follow", "assistant_message", "unrelated note")
                            for i in range(8, 25))
                rows.append(self.item(thread_id, 25, "follow", "checkpoint", "search target"))
            rows[1]["attachments"] = [
                {"id": "test-log", "name": "test.log", "mimeType": "text/plain",
                 "available": not state.get("denyAttachment"), "content": "Test 7 failed\n" + "x" * 9000},
                {"id": "reference-only", "name": "report.pdf", "mimeType": "application/pdf"}]
            assert args["maxCharsPerItem"] <= 2730
            if "beforePosition" in args and args["maxCharsPerItem"] != 1:
                assert args["view"] == "messages" and args["limit"] <= 20 and args["maxCharsPerItem"] <= 240
            if args["maxCharsPerItem"] != 1:
                for row in rows:
                    for attachment in row.get("attachments", []):
                        attachment.pop("content", None)
        if "itemId" in args:
            if args["itemId"] == "unicode":
                text = "界" + "🙂" * 3000 + "犬" + "🐦" * 3000 + "\n夕焼け"
                rows = [self.item(thread_id, 8, "root", "assistant_message", text) | {"itemId": "unicode"}]
            elif args["itemId"] == "dense-bmp":
                rows = [self.item(thread_id, 8, "root", "assistant_message", "界" * 7000) |
                        {"itemId": "dense-bmp"}]
            elif args["itemId"] == "translation":
                rows = [self.item(thread_id, 8, "root", "assistant_message", "—" * 2800) | {"itemId": "translation"}]
            else:
                rows = [row for row in rows if row["itemId"] == args["itemId"]]
        elif args["view"] == "messages":
            rows = [row for row in rows if row["type"] in ("user_message", "assistant_message")]
        lower = args.get("afterPosition", -1)
        upper = args.get("beforePosition", float("inf"))
        matches = [row for row in rows if lower < row["position"] < upper]
        pagination_test_page_size = 20 if "beforePosition" in args and scenario == "session-read" else 2
        limit = min(args.get("limit", 30), pagination_test_page_size)
        page = matches[-limit:] if "beforePosition" in args else matches[:limit]
        for row in page:
            text = row["text"].encode("utf-16-le")
            offset = args.get("textOffset", 0)
            limit_chars = args["maxCharsPerItem"]
            start = 2 * offset
            end = min(start + 2 * limit_chars, len(text))
            if end < len(text) and 0xD800 <= int.from_bytes(text[end - 2:end], "little") <= 0xDBFF:
                end -= 2
                if row["itemId"] == "unicode":
                    state.setdefault("effects", {}).setdefault("unicode_split_avoided", []).append({
                        "offset": offset, "limit": limit_chars, "nextOffset": end // 2})
            row["textTruncated"] = end < len(text)
            row["nextTextOffset"] = end // 2 if row["textTruncated"] else None
            row["text"] = text[start:end].decode("utf-16-le")
        return {"thread": summary, "recentRuns": recent, "items": page, "hasMore": len(matches) > limit,
                "nextPosition": page[-1]["position"] if page else None,
                "earlierPosition": page[0]["position"] if "beforePosition" in args and len(matches) > limit else None}

    @staticmethod
    def summary(thread_id):
        return {"threadId": thread_id, "projectId": "project", "link": "t3://" + thread_id,
                "createdBy": "agent", "creationSource": "mcp"}

    @staticmethod
    def item(thread_id, position, run_id, kind, text, source="provider", status="completed"):
        return {"position": position, "itemId": f"item-{position}", "type": kind, "text": text,
                "runId": run_id, "sourceThreadId": thread_id, "visibility": "local",
                "creationSource": source, "createdBy": "user" if source == "web" else "agent",
                "status": status, "updatedAt": "2026-01-01T00:00:00.000Z"}


def test_peer_recovery(server, runner, temp):
    server.launch_accepted = threading.Event()
    server.allow_launch_disconnect = threading.Event()
    server.state["launches"] = []
    db_path = temp + "/state.db"
    command = [runner, server.origin, temp + "/creds", db_path]
    with subprocess.Popen(command + ["start"]) as process:
        try:
            deadline = time.monotonic() + 10
            while not server.launch_accepted.wait(0.05):
                assert process.poll() is None, f"Runtime exited before T3 accepted the launch: {process.returncode}"
                assert time.monotonic() < deadline, "T3 did not accept the launch"
            action_id, = server.state["launches"]
            process.kill()
            assert process.wait(timeout=5) == -signal.SIGKILL, "Runtime was not killed by SIGKILL"
        finally:
            if process.poll() is None:
                process.kill()
            server.allow_launch_disconnect.set()
    subprocess.run(command + ["recover"], timeout=15, check=True)
    assert server.state["launches"] == [action_id], "recovery resent the launch"


with tempfile.TemporaryDirectory() as temp, ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
    server.origin = f"http://127.0.0.1:{server.server_port}"
    server.state = {}
    server.peer_recovery = sys.argv[2:] == ["peer-recovery"]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        if server.peer_recovery:
            test_peer_recovery(server, sys.argv[1], temp)
            code = 0
        else:
            code = subprocess.run([sys.argv[1], server.origin, temp + "/creds"], timeout=30, check=False).returncode
    finally:
        server.shutdown()
    sys.exit(code)
