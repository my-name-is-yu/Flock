#!/usr/bin/env python3
"""Probe prompt caching and continuation with an existing Flock OAuth access token."""

import argparse
import asyncio
import json
import os
from pathlib import Path
import re
import stat
import time
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener
from uuid import uuid4

RESOURCE = "api.openai.com/v1/responses"


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def encode(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def access_token(directory):
    with os.fdopen(os.open(directory / "profile.json", os.O_RDONLY | os.O_NOFOLLOW)) as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise SystemExit("Use an owner-only Flock credential profile.")
        profile = json.load(source)
    if (profile.get("expiresAt", 0) <= time.time() * 1000 or
            "chatgpt.tokens.use.direct" not in profile.get("granted", []) or not profile.get("accessToken")):
        raise SystemExit("Use an active Flock ChatGPT profile. Run a normal Flock inference to refresh it.")
    return profile["accessToken"]


def message(role, text):
    return {"role": role, "content": [{"type": "input_text", "text": text}]}


def request(model, label):
    offset = sum(map(ord, label))
    rows = "\n".join(
        f"Record {i}: inspector passed inventory item {(i + offset) * 43} with test threshold "
        f"{i % 13} at the training warehouse." for i in range(260)
    )
    return {"model": model, "store": False, "input": [
        message("developer", f"Cache trial {label}. Read synthetic records only. Respond solely with OK."),
        message("user", rows),
    ]}


def report(arm, sequence, body, response=None, error=None):
    usage = (response or {}).get("usage") or {}
    detail = usage.get("input_tokens_details") or {}
    print(encode({"arm": arm, "sequence": sequence, "sentBytes": len(body), "accepted": response is not None,
                  "input": usage.get("input_tokens"), "cached": detail.get("cached_tokens"),
                  "writes": detail.get("cache_write_tokens"), "error": error}), flush=True)


def safe_error(status, value):
    details = value.get("error") or {}
    if not isinstance(details, dict):
        return {"status": status}
    return {"status": status, **{key: val for key in ("code", "param")
                                 if isinstance(val := details.get(key), str) and
                                 re.fullmatch(r"[a-zA-Z0-9_.:-]{1,100}", val)}}


def http(token, arm, sequence, payload, headers=None):
    body = encode({**payload, "stream": True}).encode()
    opener = build_opener(NoRedirect)
    outgoing = Request("https://" + RESOURCE, data=body, headers={
        "Authorization": "Bearer " + token, "Content-Type": "application/json",
        "Accept": "text/event-stream", **(headers or {}),
    })
    try:
        with opener.open(outgoing, timeout=90) as stream:
            data = []
            for line in stream:
                if line.startswith(b"data:"):
                    data.append(line[5:].strip())
                    continue
                if line.strip() or not data:
                    continue
                payload = b"\n".join(data)
                data.clear()
                if payload == b"[DONE]":
                    break
                event = json.loads(payload)
                if event["type"] == "response.completed":
                    response = event["response"]
                    report(arm, sequence, body, response)
                    return response
                if event["type"] in ("response.failed", "response.incomplete", "error"):
                    report(arm, sequence, body, error=safe_error(None, event.get("response") or event))
                    return None
    except HTTPError as error:
        try:
            details = json.loads(error.read(1024 * 1024))
        except ValueError:
            details = {}
        report(arm, sequence, body, error=safe_error(error.code, details))
        return None
    except (URLError, TimeoutError, ValueError) as error:
        report(arm, sequence, body, error=type(error).__name__)
        return None
    report(arm, sequence, body, error="no completion")
    return None


def routing(token, model):
    nonce = uuid4().hex[:8]
    variants = ("none", "key", "session", "client", "both")
    for repeat, order in enumerate((variants, tuple(reversed(variants))), 1):
        for arm in order:
            label = f"{repeat}/{arm}"
            key = f"flock-probe-{nonce}-{label}"
            base = request(model, nonce + label)
            headers = {}
            if arm in ("key", "both"):
                base["prompt_cache_key"] = key
            if arm in ("session", "both"):
                headers["session-id"] = key
            if arm in ("client", "both"):
                headers["x-client-request-id"] = key
            for sequence in range(1, 4):
                http(token, label, sequence,
                     {**base, "input": base["input"] + [message("user", f"Clock is {12300 + sequence}.")]},
                     headers)


def features(token, model):
    nonce = uuid4().hex[:8]
    for arm in ("explicit", "breakpoint-user", "breakpoint-developer", "identical", "implicit-append", "http-delta"):
        base = request(model, nonce + arm)
        if arm in ("explicit", "breakpoint-user", "breakpoint-developer"):
            base["input"][1]["content"][0]["prompt_cache_breakpoint"] = {"mode": "explicit"}
            if arm == "explicit":
                base["prompt_cache_options"] = {"mode": "explicit"}
            if arm == "breakpoint-developer":
                base["input"][1]["role"] = "developer"
            base["input"].append(message("user", "Clock is 12301."))
        first = http(token, arm, 1, base)
        if not first:
            continue
        if arm == "http-delta":
            more = [message("user", "Next record number is 987. Respond solely with OK.")]
            http(token, arm, 2, {**base, "input": more, "previous_response_id": first["id"]})
            http(token, arm, 3, {**base, "input": base["input"] + first["output"] + more})
            continue
        for sequence in (2, 3):
            if arm == "implicit-append":
                base["input"].append(message("user", f"Next observation is {sequence}. Respond solely with OK."))
                base["instructions"] = f"The wall clock is {sequence}."
            http(token, arm, sequence, base)


async def websocket(token, model):
    from websockets.asyncio.client import connect
    from websockets.exceptions import InvalidStatus

    arm = "websocket-delta"

    async def send(connection, sequence, payload):
        body = encode({"type": "response.create", **payload})
        await connection.send(body)
        while True:
            event = json.loads(await asyncio.wait_for(connection.recv(), timeout=90))
            if event["type"] == "response.completed":
                result = event["response"]
                report(arm, sequence, body.encode(), result)
                return result
            if event["type"] in ("error", "response.failed", "response.incomplete"):
                report(arm, sequence, body.encode(), error=safe_error(None, event.get("response") or event))
                return None

    base = request(model, arm + uuid4().hex[:8])
    try:
        async with connect("wss://" + RESOURCE, additional_headers={"Authorization": "Bearer " + token}) as connection:
            first = await send(connection, 1, base)
            if first:
                more = [message("user", "Next record number is 987. Respond solely with OK.")]
                await send(connection, 2, {**base, "input": more, "previous_response_id": first["id"]})
                await send(connection, 3, {**base, "input": base["input"] + first["output"] + more})
    except InvalidStatus as error:
        report(arm, 0, b"", error={"status": error.response.status_code})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("model")
    parser.add_argument("probe", choices=("routing", "features", "websocket"))
    args = parser.parse_args()
    token = access_token(args.profile)
    if args.probe == "websocket":
        asyncio.run(websocket(token, args.model))
    else:
        {"routing": routing, "features": features}[args.probe](token, args.model)


if __name__ == "__main__":
    main()
