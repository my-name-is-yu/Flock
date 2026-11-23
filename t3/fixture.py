"""A loopback-only v0.1.11 wire fixture for the C++ adapter tests."""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import socket
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
            for header_value in item if isinstance(item, list) else [item]:
                self.send_header(key.replace("_", "-"), header_value)
        try:
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            # Deadline tests deliberately disconnect before a delayed reply.
            pass

    def do_GET(self):
        origin = self.server.origin
        if self.path == "/fixture/effects":
            self.reply(self.server.state.get("effects", {}))
        elif self.path == "/fixture/stock":
            self.reply({key: self.server.state[key] for key in ("stockPin", "stockHead")} |
                       {key: self.server.state.get(key, {}) for key in ("stockLaunches", "stockPromptSends")})
        elif self.path == "/protected,resource":
            if self.server.state.get("oauthFailure") == "discovery":
                self.reply({"error": "invalid_resource"}, code=400)
                return
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
            if state.get("oauthFailure") == "token":
                self.reply({"error": "invalid_grant"}, code=400)
                return
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
            if args.get("advanceStockWorker"):
                assert self.server.stock_thread
                subprocess.run(["/usr/bin/git","-C",state["stockTree"],"checkout","--detach",state["stockHead"]],
                               stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
            if "stockTrackedEdit" in args:
                assert self.server.stock_thread
                source = state["stockSource"]
                baseline = state["stockSourceContents"]
                with open(source,"w",encoding="utf-8") as tracked:
                    tracked.write(baseline)
                subprocess.run(["/usr/bin/git","-C",state["stockTree"],"add","tracked.txt"],check=True)
                mode = args["stockTrackedEdit"]
                if mode in ("staged","unstaged"):
                    with open(source,"w",encoding="utf-8") as tracked:
                        tracked.write("edited " + mode + "\n")
                    if mode == "staged":
                        subprocess.run(["/usr/bin/git","-C",state["stockTree"],"add","tracked.txt"],check=True)
                else:
                    assert mode == "clean"
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
            if state.get("oauthFailure") == "registration":
                self.reply({"error": "invalid_client_metadata"}, code=400)
                return
            assert args["token_endpoint_auth_method"] == "none" and args["grant_types"] == ["authorization_code"]
            state["registered"] = args["redirect_uris"]
            self.reply({"client_id": "fixture-client"}, code=201)
        elif self.path == "/mcp":
            if self.headers.get("Authorization") != "Bearer fixture.signed-token" or state.get("revoked"):
                challenge = ('Basic resource_metadata="https://wrong.test/", Negotiate YWJjZA==, '
                             'bEaReR realm="a, \\"b\\"", RESOURCE_METADATA \t= "' + origin +
                             '/protected,resource", Basic realm="next"')
                self.reply(code=401, WWW_Authenticate=[challenge, 'Basic realm="last"'])
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
            if tool["name"] == "t3_project_read" and state.get("projectError"):
                if state["projectError"] == "rpc":
                    envelope["error"] = {"code": -32602, "message": "Invalid params"}
                else:
                    envelope["result"] = {"isError": True, "content": [{"type": "text", "text": "Tool failed"}]}
                self.reply(envelope)
                return
            content = self.tool(tool["name"], tool["arguments"])
            if (tool["name"] == "t3_pending_request" and tool["arguments"]["action"] == "respond"
                    and state.get("dropAnswerReply")):
                self.close_connection = True
                return
            if tool["name"] == "t3_thread_send" and state.get("stockDropSendReply"):
                self.close_connection = True
                return
            envelope["result"] = {"content": [{"type": "text", "text": json.dumps(content)}],
                                  "isError": content.get("_tag") == "OrchestratorMcpFailure"}
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
        questions = [
            {"id": "color", "header": "Colors", "question": "Which colors?", "multiSelect": True,
             "allowCustomAnswer": False, "options": [{"label": "Green", "value": "green", "description": ""},
                                                     {"label": "　blue　", "description": ""}]},
            {"id": "note", "header": "Note", "question": "Any notes?", "options": [], "required": False},
            {"id": "message", "header": "Message", "question": "Edit the message", "options": [],
             "initialAnswer": "Proposed message"},
            {"id": "tag", "header": "Tag", "question": "Which tag?", "allowCustomAnswer": False,
             "options": [{"label": "Submit empty value", "value": "", "description": ""}]},
        ]
        if (name in ("t3_project_read", "t3_thread_read", "t3_thread_list", "t3_thread_launch",
                     "t3_thread_send", "t3_thread_interrupt") or
                (name == "t3_pending_request" and args["action"] == "respond")):
            state.setdefault("effects", {}).setdefault(name, []).append(args)
        assert "environmentId" not in args
        if self.server.stock_thread:
            return self.stock_tool(name, args)
        if name == "t3_environment_read":
            return {"environmentId": state.get("claimedEnvironment", "env-1"), "serverVersion": "0.1.11"}
        if name == "t3_project_read":
            if args["projectId"] == "slow-search-project":
                time.sleep(4)
                return {"id": "slow-search-project", "title": "Release Pipeline"}
            assert args["projectId"] == "project"
            failure = state.get("projectReadFailure")
            if failure is not None:
                return {"_tag": "OrchestratorMcpFailure", **failure}
            return {"id": "project", "code": "project-code", "title": "Release Pipeline", "defaultModelSelection": None}
        if name == "t3_thread_launch":
            assert args["projectId"] == "project"
            assert args["message"].endswith(f"\n\n[Flock action ID: {args['clientRequestId']}]")
            if self.server.peer_recovery:
                state["launches"].append(args["clientRequestId"])
                assert len(state["launches"]) == 1, "Runtime sent a second launch"
                assert "Verify the fixture" in args["message"]
            state["launched"] = args["message"]
            return {"projectId": "project", "threadId": "thread", "link": "t3://thread",
                    "runId": None if state.get("delayRunId") else "root"}
        if name == "t3_thread_send" and args["mode"] == "steer":
            assert scenario in ("running", "resumed") and args["threadId"] == "thread"
            assert args["clientRequestId"] == "flock-turn-7-2"
            return {"threadId": "thread", "runId": state.get("steerReply") or ("continued" if scenario == "resumed" else "root"),
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
            if state.get("waitFailure"):
                return {"_tag": "OrchestratorMcpFailure", "code": "invalid_request", "message": "Wait is unavailable."}
            if scenario == "resumed":
                return {"threadId": "thread", "runId": "continued", "timedOut": True, "status": "running"}
            active = scenario in ("running", "question", "ambiguous", "approval", "race")
            timed_out = active or scenario in ("settling", "settling-input", "manual-active")
            terminal_status = state.get("terminalStatus", "completed")
            reply = {"threadId": "thread", "runId": "root", "timedOut": timed_out,
                     "status": "running" if scenario == "running" else "waiting" if active else terminal_status}
            if not timed_out and args["untilDelegatedWorkSettled"]:
                reply["resultRun"] = {"runId": "later" if scenario.endswith("-result") else "root",
                                      "status": terminal_status}
            return reply
        if name == "t3_thread_interrupt":
            assert scenario == "running" and args == {"threadId": "thread", "runId": "root"}
            status = state.get("interruptReply") or "interrupt_requested"
            return {"threadId": "thread", "runId": None if status == "no_active_run" else "root", "status": status}
        if name == "t3_pending_request":
            assert scenario != "settling-input", "terminal Run cannot own a pending question"
            action = args["action"]
            if action == "list":
                return {"requestIds": ["request"] if scenario in ("question", "ambiguous", "race") else []}
            assert args["requestId"] == "request" and scenario in ("question", "ambiguous", "race")
            if action == "read":
                if state.get("failAnswerRecheck"):
                    reads = state.get("answerReads", 0) + 1
                    state["answerReads"] = reads
                    if reads == 2:
                        return {"_tag": "OrchestratorMcpFailure", "code": "internal_error"}
                return {"requestId": "request", "questions": questions}
            assert action == "respond" and scenario != "ambiguous"
            state["scenario"] = "running"
            if scenario == "race":
                return {"_tag": "OrchestratorMcpFailure", "code": "invalid_request",
                        "message": "The user-input request was already answered."}
            return {"sequence": 10}
        assert name == "t3_thread_read", name
        if (("beforePosition" in args and state.get("reportReadFailure") == args["view"]) or
                ("afterPosition" in args and state.get("ownershipReadFailure"))):
            return {"_tag": "OrchestratorMcpFailure", "code": "invalid_request", "message": "History is unavailable."}
        thread_id = args["threadId"]
        active = {"manual-active": "unrelated", "running": "root", "resumed": "continued"}.get(scenario)
        latest = "later" if scenario.endswith("-result") else (active or "root")
        count = 1 if scenario in ("question", "ambiguous", "approval", "race", "settling-input") else 0
        summary = self.summary(thread_id) | {"activeRunId": active, "latestRunId": latest, "archived": False,
                                             "pendingRequestCount": count}
        recent = [{"runId": "root", "ordinal": 1, "status": "completed"}]
        rows = [self.item(thread_id, 0, "root", "user_message", state["launched"], source="mcp"),
                self.item(thread_id, 1, "root", "assistant_message", "root report")]
        if scenario == "paged-report":
            rows[1]["text"] = "x" * 7000 + "end of report"
            rows.extend(self.item(thread_id, i, "other", "assistant_message", "unrelated report") for i in range(2, 5))
            if "itemId" in args and state.get("changeReportDuringRead"):
                rows[1]["updatedAt"] = "2026-11-22T00:00:01.000Z"
        if scenario.endswith("-result"):
            self.result_history(thread_id, scenario, recent, rows)
        if scenario in ("question", "ambiguous", "race"):
            rows.append(self.item(thread_id, 5, "root", "user_input_request", json.dumps(questions), status="waiting"))
        if scenario == "ambiguous":
            rows.append(self.item(thread_id, 6, "later", "user_input_request", json.dumps(questions)))
        if "followup" in state:
            rows.append(self.item(thread_id, 7, "follow", "user_message", state["followup"], source="mcp"))
        if scenario == "sparse-session-search":
            if thread_id in state.get("missingSearchThreads", []):
                return {"_tag": "OrchestratorMcpFailure", "code": "thread_not_found",
                        "message": "The thread was not found."}
            title = state["searchTitles"].get(thread_id, "Background investigation")
            summary |= {"title": title, "status": "failed", "createdAt": "2026-11-21T01:00:00Z",
                        "updatedAt": "2026-11-21T13:00:00Z"}
            delay = state.get("searchReadDelays", {}).get(thread_id, 0)
            if thread_id == state.get("slowSearchThread"):
                delay = 1.6
                summary["projectId"] = "slow-search-project"
            if delay:
                time.sleep(delay)
            rows = [self.item(thread_id, 0, "root", "user_message", "Check the logs", source="web")]
        if scenario == "session-read":
            failure = state.get("threadReadErrors", {}).get(thread_id)
            if failure is not None:
                return {"_tag": "OrchestratorMcpFailure", **failure}
            if thread_id not in ("deleted", "manual"):
                return {"_tag": "OrchestratorMcpFailure", "code": "thread_not_found",
                        "message": "The thread was not found."}
            if state.get("denyRead"):
                return {"_tag": "OrchestratorMcpFailure", "code": "capability_denied", "message": "Read scope required."}
            title = "Former investigation" if thread_id == "deleted" else "Staging investigation"
            summary |= {"title": title, "status": "failed", "runCount": 4, "archived": True,
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
            row["textTruncated"] = end < len(text)
            row["nextTextOffset"] = end // 2 if row["textTruncated"] else None
            row["text"] = text[start:end].decode("utf-16-le")
        return {"thread": summary, "recentRuns": recent, "items": page, "hasMore": len(matches) > limit,
                "nextPosition": page[-1]["position"] if page else None,
                "earlierPosition": page[0]["position"] if "beforePosition" in args and len(matches) > limit else None}

    def stock_tool(self, name, args):
        state = self.server.state
        if name == "t3_environment_read":
            return {"environmentId": state.get("claimedEnvironment", "env-1")}
        if name == "t3_project_read":
            return {"id": "project", "workspaceRoot": state["stockRoot"]}
        if name == "t3_thread_launch":
            assert args["projectId"] == "project" and "message" not in args
            assert args["runtimeMode"] == "approval-required"
            assert args["workspaceStrategy"] == {"type": "worktree", "baseRef": state["stockPin"],
                                                 "startFromOrigin": False}
            assert set(args) == {"projectId", "title", "clientRequestId", "runtimeMode", "workspaceStrategy"}
            key = args["projectId"] + ":" + args["clientRequestId"]
            launches = state.setdefault("stockLaunches", {})
            payload = {name: args[name] for name in sorted(args)}
            if key in launches:
                assert launches[key]["payload"] == payload, "same-key launch retry changed its payload"
                launches[key]["replays"] += 1
                return launches[key]["response"]
            response = {"projectId": "project", "threadId": args["clientRequestId"], "runId": None}
            launches[key] = {"payload": payload, "response": response, "replays": 0}
            return response
        if name == "t3_thread_send":
            assert args["mode"] == "queue"
            assert set(args) == {"threadId", "message", "mode", "clientRequestId"}
            thread = args["threadId"]
            if thread == "human":
                if state.get("stockRootRecovery"):
                    assert args["message"].startswith("[Flock Bot planner | ")
                    state["stockRootInput"] = args["message"]
                else:
                    assert args["clientRequestId"] == "command-1" and args["message"] == "[Flock result] Complete."
            else:
                assert thread == "pinned-worker" and args["clientRequestId"] == "pinned-worker:start"
                assert "Check fixture" in args["message"]
            if thread != "human":
                prompts = state.setdefault("stockPromptSends", {})
                key = thread + ":" + args["clientRequestId"]
                payload = {name: args[name] for name in sorted(args)}
                if key in prompts:
                    assert prompts[key]["payload"] == payload, "same-key first-prompt retry changed its payload"
                    prompts[key]["replays"] += 1
                else:
                    prompts[key] = {"payload": payload, "replays": 0}
            return {"threadId": "wrong" if state.get("badReceipt") else thread,
                    "messageId": "sent-id", "runId": "next-run", "delivery": "queued" if thread == "human" else "started"}
        if name == "t3_thread_wait":
            if args["threadId"] == "human":
                assert args["runId"] == "next-run" and args["timeoutMs"] == 1
                assert not args["untilDelegatedWorkSettled"]
                wait = state["stockRootWait"]
                return {"threadId": "other" if wait == "wrong-thread" else "human",
                        "runId": "other-run" if wait == "wrong-run" else "next-run",
                        "status": "cancelled" if wait == "cancelled" else "failed",
                        "timedOut": wait == "timed-out"}
            assert args["threadId"] == "pinned-worker" and args["runId"] == "next-run"
            return {"threadId": "pinned-worker", "runId": "next-run", "status": "completed", "timedOut": False,
                    "resultRun": {"runId": "next-run", "status": "completed"}}
        assert name == "t3_thread_read"
        thread = args["threadId"]
        is_root = thread == "human"
        summary = self.summary(thread) | {
            "activeRunId": "root-run" if is_root else None,
            "latestRunId": "root-run" if is_root else None,
            "archived": False,
            "projectId": state.get("claimedProject", "project"),
            "worktreePath": None if is_root else state["stockWrongTree" if thread == "wrong-worker" else "stockTree"],
        }
        rows = [] if not is_root else [
            self.item("human", 3, "root-run", "user_message", "Human progress", "web") | {"messageId": "human-id"},
            self.item("other", 4, "other-run", "assistant_message", "Inherited secret") | {"visibility": "inherited"},
            self.item("human", 5, "root-run", "assistant_message", "Streaming content",
                      status="completed" if state.get("finalized") else "running") | {"messageId": "reply-id"},
            self.item("human", 6, "root-run", "user_message", "[Flock result] Complete.", "mcp") |
                {"messageId": "sent-id"},
        ]
        if is_root and state.get("stockHistory") == "late-human":
            rows = [self.item("human", position, "root-run", "assistant_message", f"Progress {position}")
                    for position in range(1, 26)]
            rows.append(self.item("human", 26, "root-run", "user_message", "Later human input", "web"))
        elif is_root and state.get("stockHistory") == "streaming-late-human":
            rows = [self.item("human", 1, "root-run", "assistant_message", "Unfinished response", status="running"),
                    self.item("human", 2, "root-run", "user_message", "Human after unfinished response", "web")]
        if is_root and state.get("stockRootRecovery"):
            rows = rows[:1]
            if "stockRootInput" in state:
                rows.append(self.item(thread, 4, "next-run", "user_message", state["stockRootInput"], "mcp") |
                            {"messageId": "sent-id"})
                summary["activeRunId"] = None
                summary["latestRunId"] = "next-run"
                if state.get("stockRootExtraHistory"):
                    rows.extend(self.item(thread, position, "later-run", "assistant_message", "Later progress")
                                for position in range(5, 26))
        if thread == "pinned-worker":
            rows = [self.item(thread, 1, "next-run", "assistant_message", "Pinned report") |
                    {"textTruncated": False, "nextTextOffset": None, "messageId": "worker-result"}]
        if is_root and args["view"] == "activity" and state.get("stockInvocation"):
            mode = state["stockInvocation"]
            summary["activeRunId"] = None if mode == "finished" else "root-run"
            arguments = state["stockArguments"]
            tool_name = "mcp__flock__invoke_flock_bot" if mode == "claude" else "flock.invoke_flock_bot"
            body = json.dumps({"toolName": tool_name, "input": arguments}, ensure_ascii=False)
            source = "other" if mode == "wrong-thread" else "human"
            item = self.item(source, 2 if state.get("stockRootRecovery") else 7, "old-run" if mode == "old-run" else "root-run",
                             "dynamic_tool", body, status="running") | {"itemId": "invocation"}
            if mode == "inherited":
                item["visibility"] = "inherited"
            if args.get("itemId") is None or args["itemId"] == item["itemId"]:
                offset = args.get("textOffset", 0)
                bounded = body[offset:offset + args["maxCharsPerItem"]]
                item["text"] = bounded
                item["textTruncated"] = offset + len(bounded) < len(body)
                item["nextTextOffset"] = offset + len(bounded) if item["textTruncated"] else None
                rows = [item]
            else:
                rows = []
        if args.get("beforePosition") is not None:
            rows = [row for row in rows if row["position"] < args["beforePosition"]]
        else:
            rows = [row for row in rows if row["position"] > args.get("afterPosition", -1)]
        page = rows[-args["limit"]:] if args.get("beforePosition") is not None else rows[:args["limit"]]
        has_more = len(rows) > len(page)
        if state.get("stockIncomplete") and args.get("afterPosition") is not None:
            has_more = True
        rows = page
        return {"thread": summary, "items": rows, "recentRuns": [], "hasMore": has_more,
                "nextPosition": rows[-1]["position"] + 1 if state.get("stockIncomplete") and rows else
                    rows[-1]["position"] if rows else None,
                "earlierPosition": None}

    def result_history(self, thread_id, scenario, recent, rows):
        def add(run, kind, text, source="provider", **fields):
            rows.append(self.item(thread_id, len(rows), run, kind, text, source=source) | fields)

        def child(run, task, title):
            add(run, "subagent", "done", subagentId=task, title=title)

        def notice(run, title):
            add(run, "notification", f'Delegated task "{title}" finished', source=None, createdBy=None)

        if scenario in ("nested-result", "reused-result"):
            recent.insert(0, {"runId": "middle", "ordinal": 2, "status": "completed"})
        recent.insert(0, {"runId": "later", "ordinal": len(recent) + 1, "status": "completed"})

        title = "Owned delegate"
        if scenario == "manual-result":
            add("later", "user_message", "manual request", source="web")
        elif scenario == "server-input-result":
            child("root", "child-task", title)
            add("later", "user_message", "Delegated task child-task [completed]", source="server")
        elif scenario == "shortened-result":
            prefix = "Read notes.md and identify which action items are assigned to "
            child("root", "child-task", prefix + "the...")
            notice("later", prefix + "the release owner…")
        else:
            child("root", "child-task", title)
            if scenario == "ambiguous-title-result":
                child("unrelated", "other-task", title)
            if scenario in ("nested-result", "reused-result"):
                notice("middle", title)
                if scenario == "nested-result":
                    title = "Review follow-up"
                    child("middle", "grandchild-task", title)
            notice("later", title)
            if scenario == "mixed-result":
                add("later", "user_message", "simultaneous manual input", source="web")
        add("later", "assistant_message", "later report")

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


def test_connection_setup(server, flock, temp):
    directory = Path(temp)
    state = directory / "flock"
    codex_config = directory / "config.toml"
    claude_skills = directory / "claude-skills"
    codex_skills = directory / "codex-skills"
    claude_state = directory / "claude-state"
    claude = directory / "claude"
    claude_argv = directory / "claude-argv"
    claude.write_text("""#!/bin/sh
printf '%s\\n' "$@" >> "$FIXTURE_CLAUDE_ARGV"
case "$2" in
    add-json)
        [ "$3" = "--scope" ] && [ "$4" = "user" ] && [ "$5" = "flock" ] && [ "$6" = "-" ] &&
        [ "$#" = 6 ] || exit 2
        cat > "$FIXTURE_CLAUDE_STATE" ;;
    remove)
        [ "$3" = "--scope" ] && [ "$4" = "user" ] && [ "$5" = "flock" ] || exit 2
        rm "$FIXTURE_CLAUDE_STATE" ;;
    get) test -f "$FIXTURE_CLAUDE_STATE" ;;
    *) exit 2 ;;
esac
""")
    claude.chmod(0o700)
    env = dict(os.environ, FLOCK_CODEX_CONFIG=str(codex_config), FLOCK_CLAUDE_CLI=str(claude),
               FLOCK_CODEX_CLI="/usr/bin/true", FLOCK_CLAUDE_SKILLS_DIR=str(claude_skills),
               FLOCK_CODEX_SKILLS_DIR=str(codex_skills), FIXTURE_CLAUDE_STATE=str(claude_state),
               FIXTURE_CLAUDE_ARGV=str(claude_argv))
    env.pop("FLOCK_WEB_IDENTITIES_FILE", None)
    original = 'model = "fixture"\n\n[mcp_servers.unrelated]\ncommand = "/usr/bin/true"\n'
    codex_config.write_text(original)
    profile = state / "bots" / "reviewer"
    profile.mkdir(parents=True)
    (profile / "config.json").write_text('{"name":"Reviewer","role":"reviewer"}')
    command = [flock, "--home", str(state)]

    with socket.socket() as choose_port:
        choose_port.bind(("127.0.0.1", 0))
        port = choose_port.getsockname()[1]
    endpoint = f"http://127.0.0.1:{port}/mcp/flock"

    def connect(environment, expected_mode, flags, rewritten, restart=False, expected_error=None):
        server.state["claimedEnvironment"] = environment
        argv_before = claude_argv.read_text() if claude_argv.exists() else ""
        with subprocess.Popen(command + ["t3", "connect", environment, server.origin + "/mcp",
                                         *flags], env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stderr, selectors.EVENT_READ)
                    assert selector.select(5), "T3 connect did not display the OAuth URL"
                    assert "authorize Flock" in process.stderr.readline(), "T3 connect failed before OAuth"
                authorization = process.stderr.readline().strip()
                request = urllib.request.Request(server.origin + "/fixture",
                    data=json.dumps({"authorization": authorization}).encode(),
                    headers={"Content-Type":"application/json"})
                with urllib.request.urlopen(request, timeout=5):
                    pass
                output, errors = process.communicate(timeout=8)
                if expected_error:
                    assert process.returncode != 0 and expected_error in errors, errors
                    return
                assert process.returncode == 0, errors
            finally:
                if process.poll() is None:
                    process.kill()
        details = json.loads(output)
        assert details["mode"] == expected_mode and details["port"] == port
        assert details["configuredProviders"] == (["codex", "claude"] if rewritten else [])
        assert ("Restart 'flock run'" in errors) == restart
        codex = codex_config.read_text()
        assert codex.startswith(original) and codex.count("[mcp_servers.flock]") == 1 and endpoint in codex
        assert codex_config.stat().st_mode & 0o777 == 0o600
        token, = re.findall(r'Authorization = "Bearer (flock-t3-[a-f0-9]{64})"', codex)
        assert token not in output and token not in errors
        assert json.loads(claude_state.read_text()) == {
            "type": "http", "url": endpoint, "headers": {"Authorization": "Bearer " + token},
        }
        argv = claude_argv.read_text()
        assert "flock-t3-" not in argv and (argv != argv_before) == rewritten
        assert token.encode() not in (state / "flock.sqlite3").read_bytes()
        for root in [claude_skills, codex_skills]:
            assert (root / "flock" / "SKILL.md").is_file()
            skill = (root / "flock-reviewer" / "SKILL.md").read_text()
            assert "Reviewer" in skill and "botId `reviewer`" in skill and token not in skill
        return token

    working_command = command
    command = [flock, "--home", str(directory / "rejected-flock")]
    for reserved in [
        '[mcp_servers . flock]\nurl = "https://example.com/mcp"\n',
        'mcp_servers.flock.url = "https://example.com/mcp"\n',
        'mcp_servers = { flock = { url = "https://example.com/mcp" } }\n',
    ]:
        codex_config.write_text(reserved)
        connect("env-1", "full", ["--port", str(port)], rewritten=False,
                expected_error="Remove the existing Codex MCP server named flock")
        assert codex_config.read_text() == reserved
    command = working_command
    codex_config.write_text(original)

    old = connect("env-1", "full", ["--port", str(port)], rewritten=True, restart=True)
    token = connect("env-1", "consult", ["--mode", "consult"], rewritten=True)
    assert token != old
    assert connect("env-2", "consult", [], rewritten=False) == token

    previous_port = port
    with socket.socket() as choose_port:
        choose_port.bind(("127.0.0.1", previous_port))
        with socket.socket() as replacement:
            replacement.bind(("127.0.0.1", 0))
            port = replacement.getsockname()[1]
    endpoint = f"http://127.0.0.1:{port}/mcp/flock"
    before_port_change = token
    token = connect("env-2", "full", ["--port", str(port), "--mode", "full"], rewritten=True, restart=True)
    assert token != before_port_change

    for option in ("--headless", "--no-web"):
        with subprocess.Popen(command + ["run", option], env=env, text=True,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as headless:
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(headless.stderr, selectors.EVENT_READ)
                    assert selector.select(5), "headless startup did not warn about the MCP endpoint"
                    assert "T3 skills cannot reach Flock" in headless.stderr.readline()
            finally:
                headless.terminate()
                headless.wait(timeout=8)

    with socket.socket() as busy:
        busy.bind(("127.0.0.1", port))
        busy.listen()
        result = subprocess.run(command + ["run"], env=env, capture_output=True, text=True, timeout=8)
        assert result.returncode != 0 and f"port {port}" in result.stderr and "in use" in result.stderr

    def ping(value):
        request = urllib.request.Request(endpoint, data=b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
                    headers={"Content-Type":"application/json", "Authorization":"Bearer " + value})
        try:
            with urllib.request.urlopen(request, timeout=5) as result:
                return result.status
        except urllib.error.HTTPError as error:
            return error.code

    with subprocess.Popen(command + ["run"], env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as daemon:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(daemon.stdout, selectors.EVENT_READ)
                assert selector.select(8), "Flock did not listen on the saved port"
                assert daemon.stdout.readline().rstrip().endswith(f":{port}")
            assert ping(old) == 401 and ping(before_port_change) == 401 and ping(token) == 200
            subprocess.run(command + ["t3", "disconnect", "env-1"], env=env, check=True,
                           timeout=5, stdout=subprocess.DEVNULL)
            assert ping(token) == 200 and endpoint in codex_config.read_text() and claude_state.exists()
            subprocess.run(command + ["t3", "disconnect", "env-2"], env=env, check=True,
                           timeout=5, stdout=subprocess.DEVNULL)
            assert ping(token) == 401
            assert codex_config.read_text() == original and not claude_state.exists()
        finally:
            daemon.terminate()
            daemon.wait(timeout=8)


with tempfile.TemporaryDirectory() as temp, ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
    server.origin = f"http://127.0.0.1:{server.server_port}"
    server.state = {}
    server.peer_recovery = sys.argv[2:] == ["peer-recovery"]
    server.stock_thread = sys.argv[2:] == ["stock-thread"]
    if server.stock_thread:
        root = temp + "/project"
        good = temp + "/worker"
        wrong = temp + "/wrong-worker"
        def git(*args):
            return subprocess.check_output(["/usr/bin/git", *args], stderr=subprocess.DEVNULL, text=True).strip()
        git("init", "-b", "main", root)
        with open(root + "/tracked.txt","w",encoding="utf-8") as tracked:
            tracked.write("pinned input\n")
        git("-C",root,"add","tracked.txt")
        commit = ["-C", root, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                  "commit", "--allow-empty", "-m"]
        git(*commit, "original input")
        pin = git("-C", root, "rev-parse", "HEAD")
        git(*commit, "advanced input")
        git("-C", root, "worktree", "add", "--detach", good, pin)
        git("-C", root, "worktree", "add", "--detach", wrong, "HEAD")
        server.state.update(stockRoot=root, stockTree=good, stockWrongTree=wrong, stockPin=pin,
                            stockSource=good + "/tracked.txt", stockSourceContents="pinned input\n",
                            stockHead=git("-C", root, "rev-parse", "HEAD"))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        if server.peer_recovery:
            test_peer_recovery(server, sys.argv[1], temp)
            code = 0
        elif sys.argv[2:] == ["connection-setup"]:
            test_connection_setup(server, sys.argv[1], temp)
            code = 0
        else:
            code = subprocess.run([sys.argv[1], server.origin, temp + "/creds"], timeout=30, check=False).returncode
    finally:
        server.shutdown()
    sys.exit(code)
