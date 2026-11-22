import assert from "node:assert/strict";
import childProcess from "node:child_process";
import { EventEmitter } from "node:events";
import { closeSync, openSync, mkdirSync, mkdtempSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import { syncBuiltinESMExports } from "node:module";
import { tmpdir } from "node:os";
import { delimiter, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const issuer = "https://auth.openai.com";

function simulateAuthorization(mode) {
  const localFetch = globalThis.fetch;
  globalThis.fetch = async url => {
    assert.equal(String(url), issuer + "/.well-known/openid-configuration");
    return Response.json({
      issuer,
      authorization_endpoint: issuer + "/authorize",
      token_endpoint: issuer + "/token",
      jwks_uri: issuer + "/jwks",
    });
  };
  childProcess.spawn = (_command, [rawUrl]) => {
    const opener = new EventEmitter();
    const authorize = new URL(rawUrl);
    const callback = new URL(authorize.searchParams.get("redirect_uri"));
    assert.equal(callback.hostname, "127.0.0.1");
    callback.searchParams.set("state", authorize.searchParams.get("state"));
    callback.searchParams.set("error", "access_denied");
    const decline = async () => {
      const result = await localFetch(callback, { redirect: "manual" });
      assert.equal(result.status, 303);
    };
    if (mode === "opener-failed") {
      queueMicrotask(() => opener.emit("exit", 1));
      setTimeout(() => { void decline(); }, 40);
    } else if (mode === "opener-fails-after-callback") {
      void decline().then(() => setTimeout(() => opener.emit("exit", 1), 40));
    } else {
      void decline().then(() => setTimeout(() => opener.emit("exit", 0), 100));
    }
    return opener;
  };
  syncBuiltinESMExports();
}

function checkLogin(executable) {
  const root = mkdtempSync(join(tmpdir(), "flock-login-"));
  try {
    const bin = join(root, "bin");
    mkdirSync(bin);
    writeFileSync(
      join(bin, "node"),
      '#!/bin/sh\nexec "$FLOCK_TEST_NODE" --import "$FLOCK_TEST_PRELOAD" "$@"\n',
      { mode: 0o700 },
    );
    for (const mode of ["opener-pending", "opener-failed", "opener-fails-after-callback"]) {
      const result = childProcess.spawnSync(executable, ["--home", join(root, mode), "model", "login"], {
        encoding: "utf8",
        timeout: 10_000,
        env: {
          ...process.env,
          PATH: [bin, process.env.PATH].filter(Boolean).join(delimiter),
          FLOCK_TEST_NODE: process.execPath,
          FLOCK_TEST_PRELOAD: scriptPath,
          FLOCK_TEST_LOGIN_MODE: mode,
        },
      });
      if (result.error) throw result.error;
      assert.equal(result.status, 1, mode + " exit code");
      assert.match(result.stderr, /^flock: .*declined/im, mode + " CLI error");
      if (mode === "opener-failed") {
        assert.match(result.stderr, /^https:\/\/auth\.openai\.com\/authorize\?/m,
          "CLI should show a manual authorization URL when the browser fails to open");
      } else if (mode === "opener-fails-after-callback") {
        assert.doesNotMatch(result.stderr, /^https:\/\/auth\.openai\.com\/authorize\?/m,
          "CLI should not show a stale URL after the callback has settled");
      }
    }
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

function transportResponse(mode) {
  if (mode === "malformed") return new Response("data: {invalid}\n\n", { headers: { "content-type": "text/event-stream" } });
  if (mode === "corrupt") return new Response(Uint8Array.of(255), { headers: { "content-type": "text/event-stream" } });
  if (mode === "disconnect") throw new TypeError("connection reset");
  if (mode === "short-stream") return new Response("data: {}\n", { headers: { "content-type": "text/event-stream" } });
  const [status, code] = mode.split("/");
  return Response.json({ error: { code } }, { status: Number(status) });
}

function checkTransport(executable) {
  const root = mkdtempSync(join(tmpdir(), "flock-transport-"));
  const lock = openSync(join(root, "lock"), "wx", 0o600);
  try {
    writeFileSync(join(root, "profile.json"), JSON.stringify({
      accessToken: "fixture", refreshToken: "fixture", granted: ["chatgpt.tokens.use.direct"],
      expiresAt: Date.now() + 3600_000,
    }), { mode: 0o600 });
    for (const [mode, transient, message] of [
      ["malformed", false, /Invalid JSON/], ["corrupt", false, /Invalid UTF-8/], ["disconnect", true, /Unable to reach/],
      ["short-stream", true, /ended before/], ["400/feature_unavailable", false, /feature_unavailable/],
      ["503/test", true, /503/], ["429/subscription_sharing_usage_limit_exceeded", false, /usage_limit/],
    ]) {
      const response = childProcess.spawnSync(process.execPath, ["--import", scriptPath,
        join(dirname(executable), "chatgpt.mjs"), "infer", root, "3"], {
        input: "{}", encoding: "utf8", stdio: ["pipe", "pipe", "pipe", lock],
        env: { ...process.env, FLOCK_TEST_TRANSPORT_MODE: mode },
      });
      assert.equal(response.status, 0, response.stderr);
      const error = JSON.parse(response.stdout);
      assert.match(error.error, message);
      assert.equal(error.transient, transient, mode);
    }
  } finally {
    closeSync(lock);
    rmSync(root, { recursive: true, force: true });
  }
}

if (realpathSync(process.argv[1]) === scriptPath) {
  if (Number(process.versions.node.split(".")[0]) < 22) process.exit(77);
  checkLogin(process.argv[2]);
  checkTransport(process.argv[2]);
} else if (process.env.FLOCK_TEST_TRANSPORT_MODE) {
  globalThis.fetch = async () => transportResponse(process.env.FLOCK_TEST_TRANSPORT_MODE);
} else {
  simulateAuthorization(process.env.FLOCK_TEST_LOGIN_MODE);
}
