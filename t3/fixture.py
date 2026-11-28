"""A loopback-only v0.1.11 wire fixture for the C++ adapter tests."""
import base64
import hashlib
import json
import os
import re
import selectors
import socket
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SERVER_VERSION = "0.1.11"


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value=None, code=200, event_stream=False, **headers):
        body = json.dumps(value).encode() if value is not None else b""
        content_type = "application/json"
        if event_stream:
            content_type = "text/event-stream"
            unrelated = {"jsonrpc": "2.0", "id": str(value["id"]), "result": {}}
            body = f"data: \n\ndata: {json.dumps(unrelated)}\n\ndata: {json.dumps(value)}\n\n".encode()
        self.send_response(code)
        self.send_header("Content-Type", content_type)
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
        elif self.path == "/fixture/attachments":
            self.reply({"messages": self.server.state.get("attachmentMessages", []),
                        "pending": len(self.server.state.get("uploads", {}))})
        elif self.path == "/fixture/routes":
            self.reply(self.server.state.get("multiRoutedTargets", []))
        elif self.path == "/.well-known/t3/environment":
            self.reply({"environmentId": self.server.state.get("descriptorEnvironment",
                         self.server.state.get("claimedEnvironment", "env-1")),
                        "label": "Local T3 fixture", "serverVersion": SERVER_VERSION})
        elif self.path == "/fixture/stock":
            self.reply({key: self.server.state[key] for key in ("stockPin", "stockHead")} |
                       {key: self.server.state.get(key, {}) for key in ("stockLaunches", "stockPromptSends")})
        elif self.path == "/fixture/lineage-sessions":
            self.reply(self.server.state.get("lineageSessions", {}))
        elif self.path == "/fixture/stop-runs":
            self.reply(self.server.state.get("stopRuns", {}))
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

    def read_body(self):
        return self.rfile.read(int(self.headers["Content-Length"]))

    def do_POST(self):
        origin = self.server.origin
        state = self.server.state
        raw = self.read_body()
        if self.path.startswith("/api/attachments/upload/"):
            assert self.headers.get("Authorization") is None
            upload = state["uploads"][self.path.rsplit("/", 1)[-1]]
            if upload["name"] == state.get("rejectUpload"):
                self.reply(code=413)
                return
            assert len(raw) == upload["sizeBytes"]
            upload["bytes"] = base64.b64encode(raw).decode()
            if state.get("moveThreadOnUpload"):
                state["selectedProject"] = "other-project"
            if state.get("archiveProjectOnUpload"):
                state["archiveProject"] = True
            self.reply(code=201)
            return
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
            self.reply({"access_token": state.get("accessToken", "fixture.signed-token"),
                        "token_type": "Bearer", "expires_in": 2592000,
                        "scope": state.get("scope", "orchestration:read orchestration:operate")})
            return
        args = json.loads(raw)
        if self.path == "/fixture":
            if args.pop("multiReset", False):
                assert self.server.multi_environment
                for key in tuple(state):
                    if key.startswith("multi") and key not in ("multiProject", "multiCommit"):
                        state.pop(key)
                state.pop("effects", None)
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
            if state.get("multiDisconnected"):
                self.reply(code=503)
                return
            if self.headers.get("Authorization") != "Bearer " + state.get("accessToken", "fixture.signed-token") or state.get("revoked"):
                challenge = ('Basic resource_metadata="https://wrong.test/", Negotiate YWJjZA==, '
                             'bEaReR realm="a, \\"b\\"", RESOURCE_METADATA \t= "' + origin +
                             '/protected,resource", Basic realm="next"')
                self.reply(code=401, WWW_Authenticate=[challenge, 'Basic realm="last"'])
                return
            envelope = {"jsonrpc": "2.0", "id": args.get("id")}
            if args["method"] == "initialize":
                assert args["params"]["protocolVersion"] == "2025-06-18"
                envelope["result"] = {"protocolVersion": "2025-06-18", "serverInfo": {"name": "T3", "version": SERVER_VERSION}}
                self.reply(envelope, Mcp_Session_Id="fixture-session")
                return
            assert self.headers["Mcp-Session-Id"] == "fixture-session"
            assert self.headers["MCP-Protocol-Version"] == "2025-06-18"
            if args["method"] == "notifications/initialized":
                self.reply(code=202)
                return
            if args["method"] == "tools/list":
                tools = {"t3_thread_launch": ["environmentId", "attachments"]}
                if state.get("attachmentTools"):
                    tools.update({"t3_attachment_prepare_upload": ["upload"],
                                  "t3_attachment_discard": ["attachmentId"],
                                  "t3_thread_send_attachments": ["threadId", "message", "attachments"]})
                envelope["result"] = {"tools": [
                    {"name": name, "inputSchema": {"type": "object", "properties": dict.fromkeys(fields, {})}}
                    for name, fields in tools.items()
                ]}
                self.reply(envelope)
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
            if tool["name"] == "t3_thread_launch" and state.get("lineageFixture"):
                mode = state.get("lineageLaunchError")
                if mode in ("invalid-params", "unknown-delegated-from"):
                    launch = tool["arguments"]
                    if mode == "invalid-params" or "delegatedFrom" in launch:
                        state.setdefault("effects", {}).setdefault("t3_thread_launch", []).append(launch)
                        envelope["error"] = {"code": -32602, "message":
                            "Unknown field: delegatedFrom" if mode == "unknown-delegated-from" else "Invalid params"}
                        self.reply(envelope, event_stream=mode == "unknown-delegated-from")
                        return
            content = self.tool(tool["name"], tool["arguments"])
            if tool["name"] == "t3_thread_send_attachments" and state.get("dropAttachmentReply"):
                self.close_connection = True
                return
            if (tool["name"] == "t3_queue" and tool["arguments"]["action"] == "cancel" and
                    tool["arguments"].get("queuedRunId") == state.get("stopDropReply") and "sequence" in content):
                state.pop("stopDropReply")
                self.close_connection = True
                return
            if tool["name"] == "t3_thread_launch" and state.get("lineageLaunchError") == "lost-response":
                self.close_connection = True
                return
            if (tool["name"] == "t3_pending_request" and tool["arguments"]["action"] == "respond"
                    and state.get("dropAnswerReply")):
                self.close_connection = True
                return
            if tool["name"] == "t3_thread_send" and state.get("selectedDropSendReply"):
                self.close_connection = True
                return
            envelope["result"] = {"content": [{"type": "text", "text": json.dumps(content)}],
                                  "isError": content.get("_tag") == "OrchestratorMcpFailure"}
            if self.server.multi_environment and tool["name"] == "t3_thread_launch":
                self.server.launch_accepted.set()
                if state.get("multiDropLaunchReply"):
                    self.close_connection = True
                    return
                if state.get("multiHoldLaunchReply"):
                    assert self.server.allow_launch_disconnect.wait(15), "crash harness did not kill the Runtime"
                    self.close_connection = True
                    return
            self.reply(envelope)
        else:
            self.reply(code=404)

    def tool(self, name, args):
        state = self.server.state
        if state.get("attachmentTools") and not self.server.stock_thread:
            if name == "t3_attachment_prepare_upload":
                assert set(args) == {"upload"}
                state.setdefault("effects", {}).setdefault(name, []).append(args)
                uploads = state.setdefault("uploads", {})
                attachment_id = f"file-{len(uploads) + 1}"
                uploads[attachment_id] = args["upload"]
                path = "/api/attachments/upload/" + attachment_id
                return {"attachmentId": attachment_id, "relativeUrl": path, "uploadUrl": self.server.origin + path}
            if name == "t3_attachment_discard":
                state["uploads"].pop(args["attachmentId"])
                return {}
            if name == "t3_thread_send_attachments" or (name == "t3_thread_launch" and "attachments" in args):
                state.setdefault("effects", {}).setdefault(name, []).append(args)
                thread = args.get("threadId", "thread")
                assert thread == "thread"
                assert "clientRequestId" not in args or name == "t3_thread_launch"
                records = state.setdefault("attachmentMessages", [])
                files = []
                for ref in args["attachments"]:
                    upload = state["uploads"][ref["id"]]
                    assert {k: v for k, v in ref.items() if k != "id"} == {k: v for k, v in upload.items() if k != "bytes"}
                    files.append({"id": ref["id"], "name": ref["name"], "mimeType": ref["mimeType"], "bytes": upload["bytes"]})
                records.append({"text": args["message"], "files": files})
                run = f"attachment-run-{len(records)}"
                return {"threadId": thread, "projectId": "project", "runId": run, "status": "running",
                        "messageId": f"attachment-message-{len(records)}"}
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
                (name == "t3_queue" and args["action"] == "cancel") or
                (name == "t3_pending_request" and args["action"] == "respond")):
            state.setdefault("effects", {}).setdefault(name, []).append(args)
        target_tools = ("t3_project_list", "t3_project_read", "t3_thread_launch", "t3_thread_send",
                        "t3_thread_wait", "t3_thread_read", "t3_thread_interrupt", "t3_thread_list",
                        "t3_thread_answer", "t3_pending_request", "t3_queue")
        expected_target = state.get("multiExpectedTarget")
        if expected_target and name in target_tools:
            assert args.get("environmentId") == expected_target
        else:
            assert "environmentId" not in args
        if self.server.stock_thread:
            return self.stock_tool(name, args)
        if self.server.multi_environment:
            return self.multi_environment_tool(name, args)
        if name == "t3_environment_read":
            return {"environmentId": state.get("claimedEnvironment", "env-1"),
                    "label": "T3 fixture", "serverVersion": SERVER_VERSION}
        if name == "t3_project_list":
            assert args["limit"] == 100
            if args["cursor"] == 0:
                return {"projects": [
                    {"id": "project", "title": "Release Pipeline",
                     "archivedAt": "2026-01-01T00:00:00Z" if state.get("archiveProject") else None,
                     "workspaceRoot": "/fixture/release"},
                    {"id": "archived", "title": "Archived Release", "archivedAt": "2026-01-01T00:00:00Z",
                     "workspaceRoot": "/fixture/archive"},
                ], "nextCursor": 100}
            assert args["cursor"] == 100
            return {"projects": [{"id": "docs", "title": "Documentation",
                                  "workspaceRoot": r"C:\Fixture\docs"}], "nextCursor": None}
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
            if state.get("lineageFixture"):
                mode = state.get("lineageLaunchError")
                if mode == "tool" or (mode == "fallback-fails" and "delegatedFrom" not in args):
                    return {"_tag": "OrchestratorMcpFailure", "code": "internal_error",
                            "message": "Unknown field: delegatedFrom"}
                if mode in ("unsupported", "fallback-fails") and "delegatedFrom" in args:
                    return {"_tag": "OrchestratorMcpFailure", "code": "invalid_request",
                            "message": "Unknown field: delegatedFrom"}
                launches = state.setdefault("lineageSessions", {})
                launch = launches.setdefault(args["clientRequestId"], {
                    "threadId": f"lineage-child-{len(launches) + 1}", "arguments": args})
                assert launch["arguments"] == args, "same-key launch changed its accepted payload"
                return {"projectId": "project", "threadId": launch["threadId"], "runId": "root"}
            state["launched"] = args["message"]
            return {"projectId": "project", "threadId": "thread", "link": "t3://thread",
                    "runId": None if state.get("delayRunId") else "root"}
        if name == "t3_thread_send":
            if state.get("lineageFixture"):
                child = next(entry for entry in state["lineageSessions"].values() if entry["threadId"] == args["threadId"])
                assert args["mode"] == "queue" and args["clientRequestId"].startswith("flock-turn-")
                child["followupRunId"] = "lineage-follow-up"
                return {"threadId": child["threadId"], "runId": child["followupRunId"], "delivery": "queued"}
            if state.get("sessionWakeFixture"):
                assert args["threadId"] == "thread" and args["mode"] == "queue"
                assert args["clientRequestId"].startswith("flock-turn-")
                state["sessionSendInput"] = args["message"]
                state["botRun"] = "bot-run-" + str(len(state["effects"]["t3_thread_send"]))
                if "stopRuns" in state:
                    state["stopRuns"][state["botRun"]] = "queued"
                return {"threadId": "thread", "runId": state["botRun"], "delivery": "queued"}
            if scenario == "selected-session":
                thread = args["threadId"]
                assert thread in ("human-selected", "flock-selected") and args["mode"] == "queue"
                assert args["message"].endswith(f"\n\n[Flock action ID: {args['clientRequestId']}]")
                state["selectedInput"] = args["message"]
                state["selectedRunStatus"] = "queued" if state.get("selectedBusy") else "running"
                state["selectedStartedAt"] = None if state.get("selectedBusy") else "2026-11-21T00:00:00.000Z"
                run = "manual-run" if state.get("selectedBadReceipt") else "selected-run"
                return {"threadId": thread, "runId": run, "messageId": "selected-message",
                        "delivery": "queued" if state.get("selectedBusy") else "started", "link": "t3://" + thread}
            assert scenario in ("settled", "participant-later") and args["mode"] == "queue" and args["threadId"] == "thread"
            assert args["clientRequestId"].startswith("flock-action-")
            state["followup"] = args["message"]
            return {"threadId": "thread", "runId": "follow", "delivery": "started", "link": "t3://thread"}
        if name == "t3_queue":
            if args["action"] == "cancel" and "stopRuns" in state:
                run_id = args["queuedRunId"]
                runs = state["stopRuns"]
                if runs.get(run_id) != "queued":
                    return {"_tag": "OrchestratorMcpFailure", "code": "orchestration_error",
                            "message": f"Run {run_id} is not queued."}
                runs[run_id] = "cancelled"
                return {"sequence": 11}
            if args["action"] == "cancel" and state.get("sessionWakeFixture"):
                if args["queuedRunId"] == state["botRun"] and state["botRunStatus"] == "queued":
                    state["botRunStatus"] = "cancelled"
                    return {"sequence": 11}
                return {"_tag": "OrchestratorMcpFailure", "code": "orchestration_error",
                        "message": "The Run is no longer queued."}
            if state.get("reconcileSessionSend") and args["action"] == "list":
                text = state["sessionSendInput"]
                entries = [{"queuedRunId": "unrelated-queued", "text": text.split("\n\n")[0], "truncated": False}]
                if state["botRunStatus"] == "queued":
                    entries.append({"queuedRunId": state["botRun"], "text": text, "truncated": False})
                return {"items": entries, "nextCursor": None}
            if scenario == "selected-session":
                assert args["threadId"] in ("human-selected", "flock-selected")
                action = args["action"]
                if action in ("read", "cancel"):
                    assert args["queuedRunId"] == "selected-run"
                if action == "cancel":
                    assert state["selectedRunStatus"] == "queued"
                    state["selectedRunStatus"] = "cancelled"
                    return {"sequence": 11}
                if state["selectedRunStatus"] == "queued":
                    text = state["selectedInput"]
                    length = 1000 if action == "list" else 16000
                    entry = {"queuedRunId": "selected-run", "text": text[:length], "truncated": len(text) > length}
                    if action == "read":
                        return entry
                    return {"items": [entry], "nextCursor": None}
                return {"items": [], "nextCursor": None}
            assert args["action"] == "list" and args["threadId"] == "thread"
            return {"items": [], "nextCursor": None}
        if name == "t3_thread_list":
            if args.get("allProjects") is True:
                assert "projectId" not in args and not args["includeSubagents"]
                if state.get("sessionWakeFixture"):
                    return {"projectId": None, "threads": [self.summary("thread")], "total": 1, "nextCursor": None}
                projects = [self.summary("first"), self.summary("second") | {"projectId": "another-project"}]
                return {"projectId": None, "threads": projects, "total": len(projects), "nextCursor": None}
            assert args["projectId"] == "project"
            if state.get("lineageFixture"):
                children = [self.summary(entry["threadId"]) for entry in state["lineageSessions"].values()
                            if args["includeSubagents"] or "delegatedFrom" not in entry["arguments"]]
                return {"projectId": "project", "threads": children, "total": len(children), "nextCursor": None}
            ids = ["thread", "duplicate"] if state.get("duplicate") else ["thread"]
            if scenario == "sparse-session-search":
                ids = (["deadline-1", "deadline-2", "deadline-3"] if state.get("searchTitles", {}).get("deadline-2")
                       else [f"candidate-{i}" for i in range(1000, 1130)])
            elif scenario == "session-read":
                ids = ["deleted", "manual"]
            elif scenario == "catalog-timeout":
                ids = [f"catalog-{i}" for i in range(51)]
            start = args.get("cursor", 0)
            if scenario == "catalog-timeout" and start and state.get("delayCatalog"):
                time.sleep(2)
            end = min(start + args.get("limit", len(ids)), len(ids))
            return {"projectId": "project", "threads": [self.summary(t) for t in ids[start:end]],
                    "total": len(ids), "nextCursor": end if end < len(ids) else None}
        if name == "t3_thread_wait":
            if state.get("lineageFixture"):
                child = next(entry for entry in state["lineageSessions"].values() if entry["threadId"] == args["threadId"])
                if args["runId"] == child.get("followupRunId"):
                    status = state.get("lineageFollowupStatus", "queued")
                else:
                    assert args["runId"] == "root", args
                    status = child.get("status", "running")
                result = {"threadId": child["threadId"], "runId": args["runId"], "status": status,
                          "timedOut": status in ("running", "queued")}
                if status == "interrupted" and args["untilDelegatedWorkSettled"]:
                    result["resultRun"] = {"runId": args["runId"], "status": status}
                return result
            if state.get("sessionWakeFixture"):
                assert args["threadId"] == "thread"
                if "stopRuns" in state:
                    run_id = args["runId"]
                    status = state["stopRuns"][run_id]
                    return {"threadId": "thread", "runId": run_id, "status": status,
                            "timedOut": status in ("running", "queued")}
                assert args["runId"] == state["botRun"]
                if error := state.get("directStopWaitError"):
                    return {"_tag": "OrchestratorMcpFailure", "code": error,
                            "message": state.get("directStopWaitErrorMessage", "")}
                if state.get("transientWakeFailure"):
                    return {"_tag": "OrchestratorMcpFailure", "code": "temporarily_unavailable"}
                status = state["botRunStatus"]
                if state.get("differentWaitRun"):
                    return {"threadId": "thread", "runId": "root", "status": "completed", "timedOut": False}
                delegated_pending = args["untilDelegatedWorkSettled"] and state.get("botRunDelegatesPending", False)
                return {"threadId": "thread", "runId": state.get("botRunContinuation") or state["botRun"],
                        "status": status,
                        "timedOut": status not in ("completed", "interrupted") or
                                    state.get("botRunTimedOut", False) or delegated_pending}
            if scenario == "selected-session":
                assert args["threadId"] in ("human-selected", "flock-selected") and args["runId"] == "selected-run"
                status = state["selectedRunStatus"]
                delegated = args["untilDelegatedWorkSettled"]
                waiting_for_thread = delegated and (state.get("selectedBusy") or state.get("selectedDelegated"))
                timed_out = (status in ("queued", "running") or bool(waiting_for_thread) or
                             bool(state.get("selectedRestartPending")))
                effective = state.get("selectedEffectiveRun") if delegated else None
                reply = {"threadId": args["threadId"], "runId": effective or args["runId"], "status": status,
                         "timedOut": timed_out}
                if delegated and not timed_out:
                    reply["resultRun"] = {"runId": args["runId"], "status": status}
                return reply
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
            if state.get("sessionWakeFixture"):
                run = state.get("botRunContinuation") or state["botRun"]
                assert args == {"threadId": "thread", "runId": run}
                assert state["botRunStatus"] == "running"
                state["botRunStatus"] = "interrupted"
                return {**args, "status": "interrupt_requested"}
            if state.get("lineageFixture"):
                child = next(entry for entry in state["lineageSessions"].values() if entry["threadId"] == args["threadId"])
                assert args["runId"] == "root" and child.get("status", "running") == "running"
                child["status"] = "interrupted"
                return {**args, "status": "interrupt_requested"}
            assert scenario == "running" and args == {"threadId": "thread", "runId": "root"}
            status = state.get("interruptReply") or "interrupt_requested"
            return {"threadId": "thread", "runId": None if status == "no_active_run" else "root", "status": status}
        if name == "t3_pending_request":
            assert scenario != "settling-input", "terminal Run cannot own a pending question"
            action = args["action"]
            if action == "list":
                if state.get("sessionWakeFixture"):
                    return {"requestIds": ["request"] if state.get("botQuestion") else []}
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
        if state.get("sessionWakeFixture") and (error := state.get("directStopReadError")):
            return {"_tag": "OrchestratorMcpFailure", "code": error,
                    "message": state.get("directStopReadErrorMessage", "")}
        if state.get("lineageFixture"):
            child = next((entry for entry in state["lineageSessions"].values() if entry["threadId"] == thread_id), None)
            if not child and thread_id not in ("lineage-parent", "lineage-unrelated"):
                return {"_tag": "OrchestratorMcpFailure", "code": "thread_not_found"}
            title = child["arguments"]["title"] if child else "Deployment planning"
            actor = {"creationSource": "mcp" if child else "web", "createdBy": "agent" if child else "user"}
            status = child.get("status", "running") if child else "running"
            summary = self.summary(thread_id) | actor | {
                "activeRunId": "root" if status == "running" else None, "latestRunId": "root",
                "archived": False, "pendingRequestCount": 0, "title": title, "status": status, "runCount": 1,
                "createdAt": "2026-11-21T01:00:00.000Z", "updatedAt": "2026-11-21T13:00:00.000Z"}
            run = {"runId": "root", "ordinal": 1, "status": status}
            prompt = child["arguments"]["message"] if child else "Parent planning message"
            # A reconciliation page uses the launch marker to identify the initial Run.
            items = [] if args["view"] == "activity" else [
                self.item(thread_id, 1, "root", "user_message", prompt, source=actor["creationSource"]) |
                {"textTruncated": False, "nextTextOffset": None}]
            if args.get("afterPosition", -1) >= 1:
                items = []
            return {"thread": summary, "recentRuns": [run], "items": items, "hasMore": False,
                    "nextPosition": items[-1]["position"] if items else None, "earlierPosition": None}
        if state.get("sessionAskFixture") and thread_id not in ("thread", "other-thread"):
            return {"_tag": "OrchestratorMcpFailure", "code": "thread_not_found"}
        active = {"manual-active": "unrelated", "running": "root", "resumed": "continued"}.get(scenario)
        latest = "later" if scenario.endswith("-result") or scenario == "participant-later" else (active or "root")
        count = 1 if scenario in ("question", "ambiguous", "approval", "race", "settling-input") else 0
        if state.get("sessionWakeFixture"):
            if state["botRunStatus"] == "running":
                active = state.get("botRunContinuation") or state.get("botRun")
            if state.get("personFinished") and state["botRunStatus"] == "queued":
                active = None
        summary = self.summary(thread_id) | {"activeRunId": active, "latestRunId": latest,
                                             "archived": state.get("threadArchived", False),
                                             "pendingRequestCount": count}
        if state.get("sessionWakeFixture"):
            summary |= {"title": "Staging follow-up", "status": "running", "runCount": 2,
                        "createdAt": "2026-11-21T01:00:00.000Z", "updatedAt": "2026-11-21T13:00:00.000Z"}
        recent = [{"runId": "root", "ordinal": 1, "status": "completed"}]
        if "stopRuns" in state:
            recent = [{"runId": run, "ordinal": ordinal, "status": status,
                       "startedAt": "2026-11-21T00:00:00.000Z" if status in ("running", "completed") else None}
                      for ordinal, (run, status) in enumerate(state["stopRuns"].items(), 2)] + recent
        elif state.get("sessionAskFixture"):
            recent += [{"runId": f"bot-run-{i}", "ordinal": i + 1, "status": "completed",
                        "startedAt": "2026-11-21T00:00:00.000Z"}
                       for i in range(1, len(state["effects"].get("t3_thread_send", [])))]
        rows = [self.item(thread_id, 0, "root", "user_message", state["launched"], source="mcp"),
                self.item(thread_id, 1, "root", "assistant_message", "root report")]
        if state.get("reconcileSessionSend") and state["botRunStatus"] == "running":
            text = state["sessionSendInput"]
            rows.extend([self.item(thread_id, 2, state["botRun"], "user_message", text, source="mcp"),
                         self.item(thread_id, 3, "unrelated-started", "user_message", text, source="web")])
        if state.get("sessionWakeFixture") and state.get("botRunContinuation"):
            rows.append(self.item(thread_id, 2, state["botRunContinuation"], "user_message",
                                  "Continue where you left off.", source="server") |
                        {"messageId": "message:restart-continuation:" + state["botRun"]})
        if scenario == "selected-session":
            assert thread_id in ("human-selected", "flock-selected")
            source = "web" if thread_id == "human-selected" else "mcp"
            summary |= {"activeRunId": "manual-run" if state.get("selectedBusy") else None,
                        "latestRunId": "manual-run", "pendingRequestCount": 1 if state.get("selectedBusy") else 0,
                        "projectId": state.get("selectedProject", "project"),
                        "archived": state.get("selectedArchived", False),
                        "creationSource": source, "createdBy": "user" if source == "web" else "agent"}
            run = {"runId": "selected-run", "ordinal": 2, "status": state.get("selectedRunStatus")}
            if "selectedStartedAt" in state and not state.get("selectedOmitStart"):
                run["startedAt"] = state["selectedStartedAt"]
            recent = [run, {"runId": "manual-run", "ordinal": 1, "status": "running", "startedAt": None}]
            rows = [self.item(thread_id, 0, "manual-run", "user_message", "Earlier work", source=source)]
            if state.get("selectedRunStatus") == "running":
                rows.append(self.item(thread_id, 1, "selected-run", "user_message", state["selectedInput"], source="mcp"))
        if scenario == "paged-report":
            summary["createdAt"] = state.get("reportSessionCreatedAt", "2026-11-21T01:00:00.000Z")
            rows[1]["text"] = "x" * 7000 + "end of report"
            rows.extend(self.item(thread_id, i, "other", "assistant_message", "unrelated report") for i in range(2, 5))
            if "itemId" in args and state.get("changeReportDuringRead"):
                rows[1]["updatedAt"] = "2026-11-22T00:00:01.000Z"
        if scenario.endswith("-result"):
            self.result_history(thread_id, scenario, recent, rows)
        if scenario == "participant-later":
            # An invited Bot posted after the owned Run settled, which started a newer Run.
            recent.insert(0, {"runId": "later", "ordinal": len(recent) + 1, "status": "completed"})
            rows.append(self.item(thread_id, 2, "later", "user_message", "Invited Bot reply", source="mcp"))
        if scenario in ("question", "ambiguous", "race"):
            rows.append(self.item(thread_id, 5, "root", "user_input_request", json.dumps(questions), status="waiting"))
        if scenario == "ambiguous":
            rows.append(self.item(thread_id, 6, "later", "user_input_request", json.dumps(questions)))
        if "followup" in state:
            rows.append(self.item(thread_id, 7, "follow", "user_message", state["followup"], source="mcp"))
        if state.get("attachmentTools"):
            rows = []
            for i, message in enumerate(state.get("attachmentMessages", []), start=1):
                item = self.item(thread_id, i - 1, f"attachment-run-{i}", "user_message", message["text"], source="mcp")
                if not state.get("hideAttachmentEvidence"):
                    item["attachments"] = [{k: v for k, v in file.items() if k != "bytes"} for file in message["files"]]
                rows.append(item)
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
        page_size = 20 if "beforePosition" in args and scenario == "session-read" else 2
        return self.history_page(summary, recent, rows, args, page_size)

    @staticmethod
    def history_page(summary, recent, rows, args, page_size):
        lower = args.get("afterPosition", -1)
        upper = args.get("beforePosition", float("inf"))
        matches = [row for row in rows if lower < row["position"] < upper]
        limit = min(args.get("limit", 30), page_size)
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

    def multi_environment_tool(self, name, args):
        state = self.server.state
        if "environmentId" in args:
            state.setdefault("multiRoutedTargets", []).append(args["environmentId"])
        env = state["claimedEnvironment"]
        project = state["multiProject"]
        thread, run = "shared-thread", "shared-run"
        evidence = state.get("multiEvidence", "full")
        final_run = "continued-run" if evidence == "restarted" else run
        pre_check = "git -c flock.check=before rev-parse --verify HEAD"
        post_check = "git -c flock.check=after rev-parse --verify HEAD"
        summary = self.summary(thread) | {"projectId": project, "title": "Fixture verification"}
        terminal = ("completed", "failed", "interrupted", "cancelled")
        if name == "t3_environment_links":
            return {"environmentId": env, "links": state.get("multiLinkedEnvironments", []),
                    "notListed": "No matching linked environment is available."}
        if name == "t3_environment_read":
            return {"environmentId": env, "label": env, "serverVersion": "0.1.11"}
        if name == "t3_project_read":
            assert args["projectId"] == project
            return {"id": project, "title": project}
        if name == "t3_project_list":
            return {"projects": [{"id": project, "title": project,
                                  "workspaceRoot": f"/workspace/{project}"}], "nextCursor": None}
        if name == "t3_thread_launch":
            if state.get("multiForwardingUnavailable"):
                return {"_tag": "OrchestratorMcpFailure", "code": "capability_denied",
                        "message": "Linked environments is off on this machine, so no tool can act on another machine. Leave out environmentId to work here; the user can turn it on in Settings → Labs."}
            assert args["projectId"] == project and "multiPrompt" not in state, "Run launched twice or in the wrong project"
            prompt = args["message"]
            assert prompt.endswith(f"\n\n[Flock action ID: {args['clientRequestId']}]")
            if not state.get("multiSingle"):
                if state["multiCommit"]:
                    assert args["workspaceStrategy"] == {"type": "worktree", "baseRef": state["multiCommit"],
                                                         "startFromOrigin": False}
                    assert pre_check in prompt and post_check in prompt and state["multiCommit"] in prompt
                else:
                    assert "workspaceStrategy" not in args and pre_check not in prompt and post_check not in prompt
            state["multiPrompt"] = prompt
            return {"threadId": thread, "runId": run, "projectId": project, "link": summary["link"]}
        if name == "t3_thread_send":
            assert "multiPrompt" in state and args["threadId"] == thread and args["mode"] == "queue"
            assert args["clientRequestId"].startswith("flock-turn-")
            state["multiFollowup"] = args["message"]
            return {"threadId": thread, "runId": "follow-up-run", "delivery": "queued"}
        if name == "t3_thread_list":
            assert args["projectId"] == project and args["cursor"] == 0
            threads = [summary] if "multiPrompt" in state and not state.get("multiHideLaunch") else []
            return {"projectId": project, "threads": threads, "total": len(threads), "nextCursor": None}
        assert "multiPrompt" in state and args["threadId"] == thread
        status = state.get("multiStatus", "running")
        if name == "t3_thread_wait":
            requested_run = args["runId"]
            assert requested_run in (run, "follow-up-run")
            delegated = args["untilDelegatedWorkSettled"]
            delegated_pending = state.get("multiHoldSettlement", False) or any(
                child_status not in terminal for child_status in state.get("multiDelegatedChildStatuses", []))
            timed_out = status not in terminal or (delegated and delegated_pending)
            wait_run = "continued-run" if evidence == "restarted" and requested_run == run else requested_run
            result = {"threadId": thread, "runId": wait_run, "status": status, "timedOut": timed_out}
            if delegated and not timed_out:
                result["resultRun"] = {"runId": wait_run, "status": status}
            return result
        if name == "t3_thread_interrupt":
            assert status == "running" and args["runId"] == run
            state["multiStatus"] = "interrupted"
            return {"threadId": thread, "runId": run, "status": "interrupt_requested"}
        assert name == "t3_thread_read", name
        summary |= {"activeRunId": final_run if status not in terminal else None, "latestRunId": final_run,
                    "pendingRequestCount": 0, "archived": False, "createdAt": "2026-11-21T01:00:00.000Z"}
        recent = [{"runId": run, "ordinal": 1, "status": status, "startedAt": "2026-11-21T01:00:02.000Z"}]
        rows = [self.item(thread, 1, run, "user_message", state["multiPrompt"], source="mcp")]
        if final_run != run:
            recent.append({"runId": final_run, "ordinal": 2, "status": status,
                           "startedAt": "2026-11-21T01:00:03.000Z"})
            rows.append(self.item(thread, 3, final_run, "user_message", "Continue after restart", source="server") |
                        {"messageId": f"message:restart-continuation:{run}"})
        if status in terminal:
            expected = state["multiCommit"]
            actual = state.get("multiActualCommit", expected)
            if args["view"] == "activity":
                def command(position, run_id, input, output, status="completed"):
                    if env == "env-mini":
                        input = f"/bin/zsh -lc '{input}'"
                    rows.append(self.item(thread, position, run_id, "command_execution",
                                          f"$ {input}\n{output}\n", status=status))

                if expected:
                    first, last = pre_check, post_check
                    if evidence == "duplicate-pre":
                        last = pre_check
                    elif evidence == "unmarked":
                        first = last = "git rev-parse --verify HEAD"
                    elif evidence == "reversed":
                        first, last = last, first
                    command(2, run, first, expected)
                    if evidence != "missing-post":
                        command(5, "unrelated-run" if evidence == "foreign-post" else final_run, last,
                                actual + ("\n" + "x" * 600 if evidence == "truncated-post" else ""),
                                status="failed" if evidence == "failed-post" else "completed")
                    command(6 if evidence == "post-then-work" else 4, final_run, "ctest", "Checks ran")
                    if evidence == "rechecked-mismatch":
                        command(6, final_run, last, expected)
                    if actual != expected:
                        command(7, "unrelated-run", post_check, expected)
                else:
                    command(2, run, "ctest", "Checks ran")
                if status == "failed":
                    rows.append(self.item(thread, 8, final_run, "error", f"{env} tests failed", status="failed"))
            if evidence != "missing-report":
                report = "x" * 65537 if evidence == "truncated-report" else f"{env} verification {status}"
                rows.append(self.item(thread, 9, "unrelated-run" if evidence == "foreign-report" else final_run,
                                      "assistant_message", report))
            if evidence == "empty-final-report":
                rows.append(self.item(thread, 10, final_run, "assistant_message", ""))
        rows.sort(key=lambda row: row["position"])
        if "itemId" in args:
            rows = [row for row in rows if row["itemId"] == args["itemId"]]
        return self.history_page(summary, recent, rows, args, 2)

    def stock_tool(self, name, args):
        state = self.server.state
        if name == "t3_attachment_prepare_upload":
            state.setdefault("effects", {}).setdefault(name, []).append(args)
            uploads = state.setdefault("uploads", {})
            attachment_id = f"stock-file-{len(uploads) + 1}"
            uploads[attachment_id] = args["upload"]
            path = "/api/attachments/upload/" + attachment_id
            return {"attachmentId": attachment_id, "relativeUrl": path, "uploadUrl": self.server.origin + path}
        if name == "t3_attachment_discard":
            state.setdefault("effects", {}).setdefault(name, []).append(args)
            state.setdefault("uploads", {}).pop(args["attachmentId"], None)
            return {}
        if name == "t3_thread_send_attachments":
            state.setdefault("effects", {}).setdefault(name, []).append(args)
            files = []
            for ref in args["attachments"]:
                upload = state["uploads"][ref["id"]]
                assert {key: value for key, value in ref.items() if key != "id"} == {
                    key: value for key, value in upload.items() if key != "bytes"}
                files.append({"id": ref["id"], "name": ref["name"], "mimeType": ref["mimeType"],
                              "bytes": upload["bytes"]})
            state.setdefault("attachmentMessages", []).append({"text": args["message"], "files": files})
            return {"threadId": args["threadId"], "projectId": "project", "runId": "next-run",
                    "status": "started", "messageId": "sent-id"}
        if name == "t3_environment_read":
            return {"environmentId": state.get("claimedEnvironment", "env-1"), "label": "T3 fixture"}
        if name == "t3_project_list":
            return {"projects": [{"id": "project", "archivedAt": None}], "nextCursor": None}
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
            assert thread == "pinned-worker" and args["clientRequestId"] == "pinned-worker:start"
            assert "Check fixture" in args["message"]
            prompts = state.setdefault("stockPromptSends", {})
            key = thread + ":" + args["clientRequestId"]
            payload = {name: args[name] for name in sorted(args)}
            if key in prompts:
                assert prompts[key]["payload"] == payload, "same-key first-prompt retry changed its payload"
                prompts[key]["replays"] += 1
            else:
                prompts[key] = {"payload": payload, "replays": 0}
            return {"threadId": thread, "messageId": "sent-id", "runId": "next-run", "delivery": "started"}
        if name == "t3_thread_wait":
            assert args["threadId"] == "pinned-worker" and args["runId"] == "next-run"
            return {"threadId": "pinned-worker", "runId": "next-run", "status": "completed", "timedOut": False,
                    "resultRun": {"runId": "next-run", "status": "completed"}}
        assert name == "t3_thread_read"
        thread = args["threadId"]
        summary = self.summary(thread) | {
            "activeRunId": None, "latestRunId": None, "archived": False, "projectId": "project",
            "worktreePath": state["stockWrongTree" if thread == "wrong-worker" else "stockTree"],
        }
        rows = []
        if thread == "pinned-worker":
            row = self.item(thread, 1, "next-run", "assistant_message", "Pinned report") | {
                "textTruncated": False, "nextTextOffset": None, "messageId": "worker-result"}
            if args.get("afterPosition", -1) < row["position"] < args.get("beforePosition", float("inf")):
                rows.append(row)
        return {"thread": summary, "items": rows, "recentRuns": [], "hasMore": False,
                "nextPosition": rows[-1]["position"] if rows else None, "earlierPosition": None}

    def result_history(self, thread_id, scenario, recent, rows):
        def add(run, kind, text, source="provider", **fields):
            rows.append(self.item(thread_id, len(rows), run, kind, text, source=source) | fields)

        def child(run, task, title, native=False, **fields):
            subagent = task if native else f"node:delegated-task:{task}"
            add(run, "subagent", "done", subagentId=subagent, title=title, **fields)

        def notice(run, text, **fields):
            add(run, "notification", text, source=None, createdBy=None, **fields)

        if scenario in ("nested-result", "reused-result", "partial-batch-result"):
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
            notice("later", f'Delegated task "{prefix}the release owner…" finished')
        elif scenario in ("failed-result", "stopped-result", "unlabeled-result"):
            verb = scenario.removesuffix("-result")
            status = {"failed": "failed", "stopped": "cancelled", "unlabeled": "completed"}[verb]
            child("root", "child-task", "" if verb == "unlabeled" else title, status=status)
            notice("later", "Delegated task finished" if verb == "unlabeled" else f'Delegated task "{title}" {verb}')
        elif scenario == "batch-result":
            child("root", "a", "Read notes, logs")
            child("root", "b", "b" * 100)
            notice("later", "2 delegated tasks finished: Read notes, logs, " + "b" * 79 + "…")
        elif scenario == "partial-batch-result":
            later = "2026-01-01T00:00:01.000Z"
            child("root", "a", "a")
            child("root", "b", "b", updatedAt=later)
            notice("middle", "1 of 2 delegated tasks finished: a")
            notice("later", 'Delegated task "b" finished', updatedAt=later)
        elif scenario == "late-settlement-result":
            child("root", "child-task", title, updatedAt="2026-01-01T00:00:01.000Z")
            notice("later", f'Delegated task "{title}" finished')
        elif scenario == "foreign-child-result":
            child("unrelated", "child-task", title)
            notice("later", f'Delegated task "{title}" finished')
        elif scenario == "native-subagent-result":
            child("root", "toolu_native", "Native helper", native=True)
            child("unrelated", "child-task", title)
            notice("later", f'Delegated task "{title}" finished')
        elif scenario in ("batch-overcount-result", "invalid-batch-count-result", "zero-batch-count-result",
                          "malformed-batch-count-result"):
            child("root", "child-task", title)
            count = {"batch-overcount-result": "2", "invalid-batch-count-result": "2 of 1",
                     "zero-batch-count-result": "0", "malformed-batch-count-result": "1x"}[scenario]
            notice("later", f"{count} delegated tasks finished")
        else:
            child("root", "child-task", title)
            if scenario == "ambiguous-title-result":
                child("unrelated", "other-task", title)
            if scenario in ("nested-result", "reused-result"):
                notice("middle", f'Delegated task "{title}" finished')
                if scenario == "nested-result":
                    title = "Review follow-up"
                    child("middle", "grandchild-task", title)
            notice("later", f'Delegated task "{title}" finished')
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


def test_multi_environment(local, runner, temp, linked_routes=False):
    commit = "0123456789abcdef0123456789abcdef01234567"
    with ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as mini:
        mini.origin = f"http://127.0.0.1:{mini.server_port}"
        mini.state = {}
        mini.stock_thread = False
        mini.multi_environment = True
        for server, name in ((local, "local"), (mini, "mini")):
            server.state.update(claimedEnvironment=f"env-{name}", multiProject=f"project-{name}", multiCommit=commit)
            server.launch_accepted = threading.Event()
            server.allow_launch_disconnect = threading.Event()
        threading.Thread(target=mini.serve_forever, daemon=True).start()
        command = [runner, local.origin, mini.origin, temp + "/creds", commit]
        try:
            if linked_routes:
                subprocess.run(command + ["linked-routes"], timeout=30, check=True)
                return
            subprocess.run(command + ["normal"], timeout=30, check=True)
            local.launch_accepted.clear()
            mini.launch_accepted.clear()
            with subprocess.Popen(command + ["crash"]) as process:
                try:
                    deadline = time.monotonic() + 10
                    while not (local.launch_accepted.is_set() and mini.launch_accepted.is_set()):
                        assert process.poll() is None, f"Runtime exited before both launches were accepted: {process.returncode}"
                        assert time.monotonic() < deadline, "One environment did not accept its Run"
                        time.sleep(0.01)
                    with sqlite3.connect(Path(temp) / "recovery.db") as db:
                        actions = db.execute("""
                            SELECT id,environment_id,project_id,phase FROM outbox WHERE tool='delegate'
                        """).fetchall()
                    assert len(actions) == 2, actions
                    for server in (local, mini):
                        launch, = server.state["effects"]["t3_thread_launch"]
                        expected = (launch["clientRequestId"], server.state["claimedEnvironment"], launch["projectId"])
                        saved, = [action for action in actions if action[:3] == expected]
                        assert saved[3] in ("started", "accepted")
                        if server is mini:
                            assert saved[3] == "started", "lost response unexpectedly confirmed before Flock died"
                    process.kill()
                    assert process.wait(timeout=5) == -signal.SIGKILL, "Runtime was not killed by SIGKILL"
                finally:
                    if process.poll() is None:
                        process.kill()
                    mini.allow_launch_disconnect.set()
            subprocess.run(command + ["recover"], timeout=20, check=True)
            subprocess.run(command + ["native-start"], timeout=30, check=True)
            subprocess.run(command + ["native-resume"], timeout=30, check=True)
            subprocess.run(command + ["native-unknown"], timeout=30, check=True)
        finally:
            mini.shutdown()


def test_connection_setup(server, flock, temp):
    directory = Path(temp)
    state = directory / "flock"
    codex_config = directory / "config.toml"
    claude_config = directory / ".claude.json"
    original_claude = b'{ "z-last" : [1,  2], "mcpServers":{}, "a-first" : true }\n'
    claude_config.write_bytes(original_claude)
    claude = directory / "claude"
    claude.write_text(
        f"#!{sys.executable}\n"
        + """import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
if args == ["--version"]:
    print(os.environ.get("FIXTURE_CLAUDE_VERSION", "2.1.327") + " (Claude Code)")
    sys.exit(0)

directory = Path(os.environ.get("CLAUDE_CONFIG_DIR", os.environ["HOME"]))
path = directory / ".claude.json"
config = path.read_bytes()
servers = json.loads(config)["mcpServers"]
before = json.dumps(servers).encode()
if args == ["mcp", "add-json", "--scope", "user", "flock", "-"]:
    assert "flock" not in servers
    server = json.load(sys.stdin)
    if server["headers"]["Authorization"] != os.environ.get(
        "FIXTURE_CLAUDE_ALLOW_BEARER", server["headers"]["Authorization"]
    ):
        sys.exit(1)
    servers["flock"] = server
else:
    assert args == ["mcp", "remove", "--scope", "user", "flock"]
    del servers["flock"]
marker = b'"mcpServers":'
old = marker + before
assert old in config
path.write_bytes(config.replace(old, marker + json.dumps(servers).encode(), 1))
"""
    )
    claude.chmod(0o700)
    env = dict(os.environ, HOME=str(directory), FLOCK_CODEX_CONFIG=str(codex_config),
               FLOCK_CLAUDE_CONFIG=str(claude_config),
               FLOCK_CLAUDE_CLI=str(claude), FLOCK_CODEX_CLI="/usr/bin/true")
    env.pop("CLAUDE_CONFIG_DIR", None)
    env.pop("FLOCK_WEB_IDENTITIES_FILE", None)
    skill_paths = (directory / ".claude" / "skills", directory / ".agents" / "skills")
    env.pop("FLOCK_CLAUDE_SKILLS_DIR", None)
    env.pop("FLOCK_CODEX_SKILLS_DIR", None)
    t3_home = directory / "t3-fixture-home"
    env["T3CODE_HOME"] = str(t3_home)
    runtime_path = t3_home / "userdata" / "server-runtime.json"
    runtime_path.parent.mkdir(parents=True)
    runtime_path.write_text(json.dumps({"version": 1, "pid": os.getpid(), "port": server.server_port,
                                       "origin": server.origin, "serverVersion": "0.1.11"}))
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

    def approve(authorization):
        request = urllib.request.Request(server.origin + "/fixture",
            data=json.dumps({"authorization": authorization}).encode(),
            headers={"Content-Type":"application/json"})
        with urllib.request.urlopen(request, timeout=5):
            pass

    def connect(environment, flags, restart=False, expected_error=None, rewritten=True, source="url"):
        server.state["claimedEnvironment"] = environment
        target = {"url": ["connect", server.origin + "/mcp"], "local": ["connect"],
                  "saved": ["reconnect", environment]}[source]
        with subprocess.Popen(command + ["t3", *target, *flags], env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stderr, selectors.EVENT_READ)
                    assert selector.select(5), "T3 connect did not display the OAuth URL"
                    assert "authorize Flock" in process.stderr.readline(), "T3 connect failed before OAuth"
                authorization = process.stderr.readline().strip()
                approve(authorization)
                output, errors = process.communicate(timeout=8)
                if expected_error:
                    assert process.returncode != 0 and expected_error in errors, (output, errors)
                    return
                assert process.returncode == 0, errors
            finally:
                if process.poll() is None:
                    process.kill()
        details = json.loads(output)
        assert "mode" not in details and details["port"] == port and details["environmentId"] == environment
        has_claude = env["FLOCK_CLAUDE_CLI"] == str(claude)
        providers = ["codex", "claude"] if has_claude else ["codex"]
        assert details["configuredProviders"] == (providers if rewritten else [])
        assert ("Restart 'flock run'" in errors) == restart
        codex = codex_config.read_text()
        assert codex.startswith(original) and codex.count("[mcp_servers.flock]") == 1 and endpoint in codex
        assert codex_config.stat().st_mode & 0o777 == 0o600
        token, = re.findall(r'Authorization = "Bearer (flock-t3-[a-f0-9]{64})"', codex)
        assert token not in output and token not in errors
        if has_claude:
            assert json.loads(claude_config.read_text())["mcpServers"]["flock"] == {
                "type": "http", "url": endpoint, "headers": {"Authorization": "Bearer " + token},
            }
        assert token.encode() not in (state / "flock.sqlite3").read_bytes()
        assert not any(path.exists() for path in skill_paths), "CLI setup created skill directories"
        return token

    working_command = command
    command = [flock, "--home", str(directory / "rejected-flock")]
    for reserved in [
        '[mcp_servers . flock]\nurl = "https://example.com/mcp"\n',
        'mcp_servers.flock.url = "https://example.com/mcp"\n',
        'mcp_servers = { flock = { url = "https://example.com/mcp" } }\n',
    ]:
        codex_config.write_text(reserved)
        connect("env-1", ["--port", str(port)],
                expected_error="Remove the existing Codex MCP server named flock")
        assert codex_config.read_text() == reserved
    command = working_command
    codex_config.write_text(original)

    system_settings = state / "system-settings.json"
    system_settings.write_text(json.dumps({"port": port}))
    system_settings.chmod(0o600)
    env["FLOCK_CLAUDE_CLI"] = str(directory / "not-installed")
    old = connect("env-1", [], restart=True, source="local")
    system_settings.unlink()
    assert claude_config.read_bytes() == original_claude
    env["FLOCK_CLAUDE_CLI"] = str(claude)
    token = connect("env-2", [])
    assert token != old
    assert connect("env-2", [], rewritten=False, source="saved") == token
    assert connect("env-3", [], rewritten=False) == token
    subprocess.run(command + ["t3", "disconnect", "env-3"], env=env, check=True,
                   timeout=5, stdout=subprocess.DEVNULL)
    credential_dir = state / "credentials" / "t3"
    credentials = {path.name: path.read_bytes() for path in credential_dir.glob("*.json")}
    previous_config = codex_config.read_bytes()
    def connector():
        with sqlite3.connect(state / "flock.sqlite3") as db:
            return db.execute("SELECT * FROM t3_connector").fetchone()
    previous_connector = connector()
    previous_claude = claude_config.read_bytes()
    for setting, value, error in [
        ("FIXTURE_CLAUDE_VERSION", "2.1.326", "Claude Code 2.1.327 or later is required"),
        ("FIXTURE_CLAUDE_ALLOW_BEARER", "Bearer " + token, "Claude Code could not register the Flock MCP server"),
    ]:
        env[setting] = value
        connect("env-2", ["--port", str(port - 1)], expected_error=error)
        del env[setting]
        assert {path.name: path.read_bytes() for path in credential_dir.glob("*.json")} == credentials
        assert codex_config.read_bytes() == previous_config and connector() == previous_connector
        assert claude_config.read_bytes() == previous_claude

    previous_port = port
    with socket.socket() as choose_port:
        choose_port.bind(("127.0.0.1", previous_port))
        with socket.socket() as replacement:
            replacement.bind(("127.0.0.1", 0))
            port = replacement.getsockname()[1]
    endpoint = f"http://127.0.0.1:{port}/mcp/flock"
    before_port_change = token
    token = connect("env-2", ["--port", str(port)], restart=True)
    assert token != before_port_change

    system_settings.write_text(json.dumps({"port": port}))
    system_settings.chmod(0o600)
    configured = codex_config.read_bytes(), claude_config.read_bytes(), connector()
    conflict = subprocess.run(command + ["t3", "connect", server.origin + "/mcp",
                                        "--port", str(previous_port)],
                              env=env, text=True, capture_output=True, timeout=5)
    assert conflict.returncode != 0 and "Flock System settings select Web port" in conflict.stderr
    assert (codex_config.read_bytes(), claude_config.read_bytes(), connector()) == configured

    with subprocess.Popen(command + ["run", "--no-web"], env=env, text=True,
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as headless:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(headless.stderr, selectors.EVENT_READ)
                assert selector.select(5), "headless startup did not warn about the MCP endpoint"
                assert "Flock Session tools cannot reach Flock" in headless.stderr.readline()
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

    admin_credential = "Bearer fixture-local-owner-admin"
    member_credential = "Bearer fixture-local-owner-member"
    identities = directory / "web-identities.json"
    identities.write_text(json.dumps([
        {"person": "local-owner", "authorization": admin_credential, "adminRoot": True},
        {"person": "local-owner", "authorization": member_credential},
    ]))
    identities.chmod(0o600)
    env["FLOCK_WEB_IDENTITIES_FILE"] = str(identities)

    def settings(path="", body=None, credential=admin_credential):
        origin = f"http://127.0.0.1:{port}"
        request = urllib.request.Request(origin + "/api/settings" + path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={"Origin":origin, "Content-Type":"application/json", "X-Flock-Action":"1",
                     "Authorization":credential})
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, json.load(response)

    def await_t3_settings(predicate):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            _, value = settings()
            if predicate(value):
                return value
            assert not value["t3Setup"]["error"], value["t3Setup"]
            time.sleep(0.05)
        raise AssertionError("Flock did not finish the Web T3 connection")

    with subprocess.Popen(command + ["run"], env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as daemon:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(daemon.stdout, selectors.EVENT_READ)
                assert selector.select(8), "Flock did not listen on the saved port"
                assert daemon.stdout.readline().rstrip().endswith(f":{port}")
            assert ping(old) == 401 and ping(before_port_change) == 401 and ping(token) == 200
            assert not any(path.exists() for path in skill_paths), "Web startup created skill directories"

            server.state["claimedEnvironment"] = "env-local-web"
            server.state["descriptorEnvironment"] = "env-wrong"
            try:
                settings("/t3-connect", {"source": "local"}, member_credential)
            except urllib.error.HTTPError as error:
                assert error.code == 403
            else:
                raise AssertionError("non-admin credential started Web T3 setup")
            status, _ = settings("/t3-connect", {"source": "local"})
            assert status == 202
            pending = await_t3_settings(lambda value: value["t3Setup"]["authorizationUrl"])
            approve(pending["t3Setup"]["authorizationUrl"])
            failed = await_t3_settings(lambda value: not value["t3Setup"]["running"])
            assert failed["t3Setup"]["error"]
            assert all(row["environmentId"] != "env-local-web" for row in failed["t3"]["environments"])
            del server.state["descriptorEnvironment"]

            def connect_web(target):
                assert settings("/t3-connect", target)[0] == 202
                pending = await_t3_settings(lambda value: value["t3Setup"]["authorizationUrl"])
                approve(pending["t3Setup"]["authorizationUrl"])
                connected = await_t3_settings(lambda value: not value["t3Setup"]["running"])
                assert not connected["t3Setup"]["error"]
                return {row["environmentId"]: row["authorization"] for row in connected["t3"]["environments"]}

            authorized = connect_web({"source": "local"})["env-local-web"]
            assert authorized["label"] == "T3 fixture" and not authorized["expired"]
            assert not any(path.exists() for path in skill_paths), "Web setup created skill directories"
            credential_file = next(file for file in credential_dir.glob("*.json")
                                   if json.loads(file.read_text())["environmentId"] == "env-local-web")
            expired = json.loads(credential_file.read_text())
            expired["expiresAt"] = 1
            credential_file.write_text(json.dumps(expired))
            assert next(row["authorization"]["expired"] for row in settings()[1]["t3"]["environments"]
                        if row["environmentId"] == "env-local-web")
            assert not connect_web({"source": "saved", "environmentId": "env-local-web"})["env-local-web"]["expired"]
            assert settings("/t3-disconnect", {"environmentId":"env-local-web"})[0] == 200
            assert all(row["environmentId"] != "env-local-web" for row in settings()[1]["t3"]["environments"])
            assert ping(token) == 200

            server.state["claimedEnvironment"] = "env-from-url"
            assert "env-from-url" in connect_web({"source": "url", "mcpUrl": server.origin + "/mcp"})
            assert settings("/t3-disconnect", {"environmentId": "env-from-url"})[0] == 200

            subprocess.run(command + ["t3", "disconnect", "env-1"], env=env, check=True,
                           timeout=5, stdout=subprocess.DEVNULL)
            assert ping(token) == 200 and endpoint in codex_config.read_text() and "flock" in json.loads(claude_config.read_text())["mcpServers"]
            subprocess.run(command + ["t3", "disconnect", "env-2"], env=env, check=True,
                           timeout=5, stdout=subprocess.DEVNULL)
            assert ping(token) == 401
            assert codex_config.read_text() == original and claude_config.read_bytes() == original_claude

            connect("env-1", ["--port", str(port)], restart=True)
            second = [flock, "--home", str(directory / "second-flock")]
            command = second
            # Setup only needs OAuth and provider files. The Web port remains with the first home.
            profile = directory / "second-flock" / "bots" / "reviewer"
            profile.mkdir(parents=True)
            (profile / "config.json").write_text('{"name":"Reviewer","role":"reviewer"}')
            connect("env-1", ["--port", str(port)], restart=True)
            configs = codex_config.read_bytes(), claude_config.read_bytes()
            subprocess.run(working_command + ["t3", "disconnect", "env-1"], env=env, check=True,
                           timeout=5, stdout=subprocess.DEVNULL)
            assert (codex_config.read_bytes(), claude_config.read_bytes()) == configs

            # A leftover Claude Code config must not strand the Codex registration once Claude Code is gone.
            env["FLOCK_CLAUDE_CLI"] = str(directory / "not-installed")
            claude_config.write_text("{ not json")
            subprocess.run(second + ["t3", "disconnect", "env-1"], env=env, check=True,
                           timeout=5, stdout=subprocess.DEVNULL)
            assert codex_config.read_text() == original and claude_config.read_text() == "{ not json"
            env["FLOCK_CLAUDE_CLI"] = str(claude)
        finally:
            daemon.terminate()
            daemon.wait(timeout=8)


if sys.argv[2:3] == ["recording-contract"]:
    from recording_contract import verify
    sys.exit(verify(Fixture, sys.argv[1], Path(sys.argv[3])))

with tempfile.TemporaryDirectory() as temp, ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
    server.origin = f"http://127.0.0.1:{server.server_port}"
    server.state = {}
    server.stock_thread = sys.argv[2:] == ["pinned-workspace"]
    server.multi_environment = sys.argv[2:] in (["multi-environment"], ["multi-environment-links"])
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
        if server.multi_environment:
            test_multi_environment(server, sys.argv[1], temp, sys.argv[2:] == ["multi-environment-links"])
            code = 0
        elif sys.argv[2:] == ["connection-setup"]:
            test_connection_setup(server, sys.argv[1], temp)
            code = 0
        else:
            code = subprocess.run([sys.argv[1], server.origin, temp + "/creds"], timeout=30, check=False).returncode
    finally:
        server.shutdown()
    sys.exit(code)
