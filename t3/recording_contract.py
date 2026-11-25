import copy
import json
import os
import re
import subprocess
import tempfile
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path


class ContractError(Exception):
    pass


def require(condition, reason):
    if not condition:
        raise ContractError(reason)


def field(obj, name, kind, *, nullable=False):
    require(type(obj) is dict and name in obj, f"missing {name}")
    value = obj[name]
    if nullable and value is None:
        return None
    require(type(value) is kind, f"wrong type for {name}")
    return value


def identifier(obj, name):
    value = field(obj, name, str)
    require(value, f"empty {name}")
    return value


def rpc(exchange, method):
    request = field(exchange, "request", dict)
    response = field(exchange, "response", dict)
    require(request.get("method") == method, f"expected {method}")
    require(request.get("jsonrpc") == response.get("jsonrpc") == "2.0", "wrong JSON-RPC version")
    require(field(request, "id", int) == field(response, "id", int), "wrong JSON-RPC response ID")
    require(200 <= field(exchange, "status", int) < 300, "unsuccessful HTTP response")
    require(exchange.get("contentType") in ("application/json", "text/event-stream"), "unexpected HTTP content type")
    require("error" not in response, "unexpected JSON-RPC error")
    return field(request, "params", dict), field(response, "result", dict)


def call(exchange, name, is_error=False):
    params, result = rpc(exchange, "tools/call")
    require(params.get("name") == name, f"expected {name}")
    error = field(result, "isError", bool) if "isError" in result else False
    require(error == is_error, "unexpected MCP tool result")
    data = result.get("structuredContent")
    if data is None:
        content = field(result, "content", list)
        require(len(content) == 1 and field(content[0], "type", str) == "text", "missing MCP tool payload")
        data = json.loads(field(content[0], "text", str))
    require(type(data) is dict, "wrong type for MCP tool payload")
    return field(params, "arguments", dict), data


def validate_page(exchange, project, thread_id, run_id, cursor, has_more):
    args, page = call(exchange, "t3_thread_read")
    require(identifier(args, "threadId") == thread_id, "read targeted the wrong thread")
    require(field(args, "afterPosition", int) == cursor, "read did not use the preceding cursor")
    require(field(args, "view", str) == "messages", "expected messages view")
    limit = field(args, "limit", int)
    require(limit > 0 and field(args, "maxCharsPerItem", int) > 0 and field(args, "runLimit", int) > 0,
            "invalid read limits")
    thread = field(page, "thread", dict)
    require(identifier(thread, "threadId") == thread_id, "read returned the wrong thread")
    require(identifier(thread, "projectId") == project, "read returned the wrong project")
    require(identifier(thread, "latestRunId") == run_id, "read returned the wrong latest run")
    field(thread, "activeRunId", str, nullable=True)
    field(thread, "archived", bool)
    field(thread, "pendingRequestCount", int)
    runs = field(page, "recentRuns", list)
    require(any(identifier(run, "runId") == run_id for run in runs), "read lost the launched run")
    for run in runs:
        field(run, "ordinal", int)
        identifier(run, "status")
    items = field(page, "items", list)
    require(0 < len(items) <= limit, "read page did not respect the requested limit")
    ids = set()
    for item in items:
        require(identifier(item, "sourceThreadId") == thread_id, "item belongs to a different thread")
        require(identifier(item, "runId") == run_id, "item belongs to a different run")
        ids.add(identifier(item, "itemId"))
        require(field(item, "visibility", str) == "local", "expected local history")
        require(identifier(item, "type") in ("user_message", "assistant_message"), "expected a message item")
        field(item, "text", str, nullable=True)
        if field(item, "textTruncated", bool):
            require(field(item, "nextTextOffset", int) > 0, "text cursor did not advance")
        position = field(item, "position", int)
        require(position > cursor, "history cursor did not advance")
        cursor = position
    require(len(ids) == len(items), "read repeated an item")
    require(field(page, "hasMore", bool) == has_more, "wrong pagination state")
    require(field(page, "nextPosition", int) == cursor, "page cursor does not match its last item")
    field(page, "earlierPosition", int, nullable=True)
    return cursor, ids


def validate(exchanges):
    require(len(exchanges) == 6, "expected one initialize, launch, wait, two read pages and a read error")
    initialize, launch, wait, first, last, failed = exchanges
    args, hello = rpc(initialize, "initialize")
    require(args.get("protocolVersion") == hello.get("protocolVersion") == "2025-06-18", "MCP version mismatch")

    args, launched = call(launch, "t3_thread_launch")
    project = identifier(args, "projectId")
    identifier(args, "clientRequestId")
    identifier(args, "message")
    identifier(args, "title")
    require(identifier(launched, "projectId") == project, "launch returned the wrong project")
    thread = identifier(launched, "threadId")
    run = identifier(launched, "runId")

    args, waited = call(wait, "t3_thread_wait")
    require(identifier(args, "threadId") == identifier(waited, "threadId") == thread, "wait thread mismatch")
    require(identifier(args, "runId") == identifier(waited, "runId") == run, "wait run mismatch")
    require(field(args, "timeoutMs", int) > 0 and field(args, "untilDelegatedWorkSettled", bool), "invalid wait limits")
    require(not field(waited, "timedOut", bool) and waited.get("status") == "completed", "wait did not settle")
    if waited.get("resultRun") is not None:
        result = field(waited, "resultRun", dict)
        require(identifier(result, "runId") == run and result.get("status") == "completed", "unexpected result run")

    cursor, first_ids = validate_page(first, project, thread, run, -1, True)
    _, last_ids = validate_page(last, project, thread, run, cursor, False)
    require(not first_ids & last_ids, "pagination returned the same item twice")

    args, error = call(failed, "t3_thread_read", is_error=True)
    require(identifier(args, "threadId") != thread, "error did not target a missing thread")
    error = error.get("error", error)
    require(error.get("code") == "thread_not_found" or
            (error.get("code") == "invalid_request" and error.get("message") == "The thread was not found."),
            "expected thread-not-found error")


def run_probe(handler, program, args, setup, env):
    with ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        server.origin = f"http://127.0.0.1:{server.server_port}"
        setup(server)
        worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
        worker.start()
        try:
            subprocess.run([program, server.origin, *args], env=env, timeout=10, check=True)
        finally:
            server.shutdown()
            worker.join()


def fixture_capture(fixture, program, capture_path, commit, env):
    observed = []

    class ObservedFixture(fixture):
        def read_body(self):
            raw = super().read_body()
            self.request_envelope = json.loads(raw)
            return raw

        def reply(self, value=None, code=200, event_stream=False, **headers):
            request = getattr(self, "request_envelope", {})
            if request.get("method") in ("initialize", "tools/call"):
                event_stream = event_stream or len(observed) == 4
                content_type = "text/event-stream" if event_stream else "application/json"
                observed.append(dict(request=request, response=value, status=code, contentType=content_type))
            super().reply(value, code, event_stream, **headers)

    def setup(server):
        server.state = {"sessionAskFixture": True}
        server.peer_recovery = server.multi_environment = server.stock_thread = False

    run_probe(ObservedFixture, program, ("project", "offline-action", "missing-thread"), setup,
              dict(env, FLOCK_RECORD_T3=str(capture_path), FLOCK_RECORD_COMMIT=commit))
    return observed


def replay(fixture, program, exchanges, env):
    remaining = iter(exchanges)
    completed = []

    class Replay(fixture):
        def do_POST(self):
            request = json.loads(self.read_body())
            if request.get("method") == "notifications/initialized":
                self.reply(code=202)
                return
            exchange = next(remaining)
            expected = exchange["request"]
            for name in ("id", "jsonrpc", "method"):
                require(request.get(name) == expected[name], f"replay request {name} mismatch")
            actual = request["params"]
            expected = expected["params"]
            if request["method"] == "tools/call":
                require(actual["name"] == expected["name"], "replay tool mismatch")
                actual, expected = actual["arguments"], expected["arguments"]
            for name, value in expected.items():
                if name in ("title", "message"):
                    identifier(actual, name)
                else:
                    require(name in actual and actual[name] == value, f"replay argument {name} mismatch")
            headers = {"Mcp_Session_Id": "fixture-session"} if request["method"] == "initialize" else {}
            self.reply(exchange["response"], code=exchange["status"],
                       event_stream=exchange["contentType"] == "text/event-stream", **headers)
            completed.append(exchange)

    launch = exchanges[1]["request"]["params"]["arguments"]
    missing_thread = exchanges[-1]["request"]["params"]["arguments"]["threadId"]
    run_probe(Replay, program, (launch["projectId"], launch["clientRequestId"], missing_thread), lambda _: None, env)
    require(len(completed) == len(exchanges), "replay did not use the complete transcript")


def read_records(path):
    records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    for record in records:
        require(field(record, "schema", int) == 1, "wrong recording schema")
        require(record.get("source") in ("fixture", "real"), "missing recording provenance")
        require(re.fullmatch(r"[0-9a-fA-F]{40}", field(record, "captureCommit", str)), "missing capture commit")
        identifier(record, "t3Version")
    return records


def verify(fixture, program, recording):
    env = os.environ.copy()
    env.pop("FLOCK_RECORD_T3", None)
    exchanges = [record["exchange"] for record in read_records(recording)]
    validate(exchanges)
    replay(fixture, program, exchanges, env)

    structured = copy.deepcopy(exchanges)
    for exchange in structured[1:]:
        result = exchange["response"]["result"]
        if result.get("structuredContent") is None:
            result["structuredContent"] = json.loads(result["content"][0]["text"])
        result["structuredContent"]["extension"] = {"version": 1}
        result.pop("content", None)
    validate(structured)
    replay(fixture, program, structured, env)

    for index, reason in ((2, "missing runId"), (1, "wrong type for isError")):
        corrupt = copy.deepcopy(structured)
        result = corrupt[index]["response"]["result"]
        if index == 2:
            del result["structuredContent"]["runId"]
        else:
            result["isError"] = "false"
        try:
            validate(corrupt)
        except ContractError as error:
            require(str(error) == reason, "negative transcript failed for the wrong reason")
        else:
            raise ContractError(f"accepted a negative transcript with {reason}")

    with tempfile.TemporaryDirectory() as directory:
        capture = Path(directory) / "t3.jsonl"
        commit = "0" * 40
        observed = fixture_capture(fixture, program, capture, commit, env)
        validate(observed)
        captured = read_records(capture)
        version = observed[0]["response"]["result"]["serverInfo"]["version"]
        require(all(record["source"] == "real" and record["t3Version"] == version and record["captureCommit"] == commit
                    for record in captured), "capture lost provenance")
        captured_exchanges = [record["exchange"] for record in captured]
        validate(captured_exchanges)
        replay(fixture, program, captured_exchanges, env)
        raw_capture = capture.read_text(encoding="utf-8")
        require(all(value not in raw_capture for value in
                    ("fixture.signed-token", "fixture-session", "/Users/fixture", "private@example.invalid", "offline-action",
                     "missing-thread", "t3://thread", "root report")), "capture leaked private fixture data")
