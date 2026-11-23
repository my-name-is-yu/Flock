import assert from "node:assert/strict";
import childProcess from "node:child_process";
import { generateKeyPairSync, sign } from "node:crypto";
import { EventEmitter, once } from "node:events";
import { closeSync, existsSync, openSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import { syncBuiltinESMExports } from "node:module";
import { tmpdir } from "node:os";
import { delimiter, dirname, join } from "node:path";
import { setTimeout as pause } from "node:timers/promises";
import { fileURLToPath } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const issuer = "https://auth.openai.com";

function installNodeWrapper(root) {
  const bin = join(root, "bin");
  mkdirSync(bin);
  writeFileSync(
    join(bin, "node"),
    '#!/bin/sh\nexec "$FLOCK_TEST_NODE" --import "$FLOCK_TEST_PRELOAD" "$@"\n',
    { mode: 0o700 },
  );
  return {
    ...Object.fromEntries(Object.entries(process.env).filter(([name]) => !name.startsWith("FLOCK_"))),
    PATH: [bin, process.env.PATH].filter(Boolean).join(delimiter),
    FLOCK_TEST_NODE: process.execPath,
    FLOCK_TEST_PRELOAD: scriptPath,
  };
}

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
    const env = installNodeWrapper(root);
    for (const mode of ["opener-pending", "opener-failed", "opener-fails-after-callback"]) {
      const result = childProcess.spawnSync(executable, ["--home", join(root, mode), "model", "login"], {
        encoding: "utf8",
        timeout: 10_000,
        env: { ...env, FLOCK_TEST_LOGIN_MODE: mode },
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
      hostId: "fixture-installation", accessToken: "fixture", refreshToken: "fixture", granted: ["chatgpt.tokens.use.direct"],
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
    const invoke = (scope, input) => {
      const response = childProcess.spawnSync(process.execPath, ["--import", scriptPath,
        join(dirname(executable), "chatgpt.mjs"), "infer_conversation", root, "3"], {
        input: JSON.stringify({ request: { input }, scope }), encoding: "utf8",
        stdio: ["pipe", "pipe", "pipe", lock],
        env: { ...process.env, FLOCK_TEST_TRANSPORT_MODE: "scope" },
      });
      assert.equal(response.status, 0, response.stderr);
      const { result } = JSON.parse(response.stdout);
      assert.equal(result.transportRequestBytes, Buffer.byteLength(JSON.stringify({ input })));
      const { session, body } = result.output[0];
      assert.deepEqual(body, { input });
      assert.match(session, /^[a-f0-9]{64}$/);
      return session;
    };
    const first = invoke("private scope A", "first prompt");
    assert.equal(invoke("private scope A", "updated prompt"), first);
    assert.notEqual(invoke("private scope B", "first prompt"), first);
  } finally {
    closeSync(lock);
    rmSync(root, { recursive: true, force: true });
  }
}

function simulateAccountLifecycle(mode) {
  const localFetch = globalThis.fetch;
  const { publicKey, privateKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
  const encode = (value) => Buffer.from(JSON.stringify(value)).toString("base64url");
  const sameAccount = mode === "same-account";
  const clientId = sameAccount ? "fixture-old-registration" : "fixture-new-registration";
  let nonce;
  globalThis.fetch = async (url, options) => {
    switch (String(url)) {
      case `${issuer}/.well-known/openid-configuration`:
        return Response.json({ issuer, authorization_endpoint: `${issuer}/authorize`, token_endpoint: `${issuer}/token`,
          jwks_uri: `${issuer}/jwks`, revocation_endpoint: `${issuer}/revoke` });
      case `${issuer}/jwks`:
        return Response.json({ keys: [{ ...publicKey.export({ format: "jwk" }), kid: "test-signing-key" }] });
      case `${issuer}/token`: {
        assert.equal(options.body.get("client_id"), clientId);
        const gate = process.env.FLOCK_TEST_ACCOUNT_GATE;
        if (gate) {
          writeFileSync(join(gate, "ready"), "");
          await waitFor(() => existsSync(join(gate, "release")), "OAuth fixture did not resume");
        }
        const iat = Math.floor(Date.now() / 1000);
        const content = `${encode({ alg: "RS256", kid: "test-signing-key" })}.${encode({ iss: issuer, aud: clientId,
          sub: sameAccount ? "fixture-old-subject" : "fixture-new-subject",
          email: sameAccount ? "first@example.com" : "second@example.com", nonce, iat, exp: iat + 600 })}`;
        return Response.json({ id_token: `${content}.${sign("RSA-SHA256", Buffer.from(content), privateKey).toString("base64url")}`,
          access_token: "new-access", refresh_token: "new-refresh", token_type: "bearer", expires_in: 900,
          scope: "openid chatgpt.tokens.use.direct" });
      }
      case `${issuer}/revoke`:
        assert.equal(options.body.get("token"), "pending-refresh");
        assert.equal(options.body.get("token_type_hint"), "refresh_token");
        assert.equal(options.body.get("client_id"), "fixture-old-registration");
        return new Response(null, { status: mode === "revocation-unavailable" ? 503 : 200 });
      default:
        assert.fail("Unexpected ChatGPT account lifecycle request");
    }
  };
  childProcess.spawn = (_command, [url]) => {
    const opened = new EventEmitter();
    const authorization = new URL(url);
    assert.equal(authorization.searchParams.get("client_id"), sameAccount ? clientId : "dynamic_agent_client");
    assert.equal(authorization.searchParams.get("ext_agent_host_id"), "urn:uuid:fixture-stable-host");
    assert.equal(authorization.searchParams.get("agent_name_hint"), sameAccount ? null : "Flock");
    nonce = authorization.searchParams.get("nonce");
    const callback = new URL(authorization.searchParams.get("redirect_uri"));
    callback.searchParams.set("state", authorization.searchParams.get("state"));
    if (mode === "switch-declined") callback.searchParams.set("error", "access_denied");
    else {
      callback.searchParams.set("code", "fixture-authorization-code");
      callback.searchParams.set("client_id", clientId);
    }
    void localFetch(callback, { redirect: "manual" }).then(response => {
      assert.equal(response.status, 303);
      opened.emit("exit", 0);
    });
    return opened;
  };
  syncBuiltinESMExports();
}

const signedOutProfile = {
  hostId: "urn:uuid:fixture-stable-host", clientId: "fixture-old-registration",
  subject: "fixture-old-subject", email: "first@example.com", signedOut: true, revocationConfirmed: true,
};

function checkAccountLifecycle(executable) {
  const root = mkdtempSync(join(tmpdir(), "flock-account-lifecycle-"));
  const lock = openSync(join(root, "lock"), "wx", 0o600);
  const profile = join(root, "profile.json");
  const original = { hostId: "urn:uuid:fixture-stable-host", clientId: "fixture-old-registration", subject: "fixture-old-subject",
    email: "first@example.com", idToken: "original-id", accessToken: "original-access", refreshToken: "original-refresh",
    pending: { refreshToken: "pending-refresh" }, granted: ["chatgpt.tokens.use.direct"] };
  const writeOriginal = () => writeFileSync(profile, JSON.stringify(original), { mode: 0o600 });
  const read = () => JSON.parse(readFileSync(profile, "utf8"));
  const invoke = (operation, mode, input = {}) => {
    const response = childProcess.spawnSync(process.execPath, ["--import", scriptPath,
      join(dirname(executable), "chatgpt.mjs"), operation, root, "3"], {
      input: JSON.stringify(input), encoding: "utf8", stdio: ["pipe", "pipe", "pipe", lock], timeout: 10_000,
      env: { ...process.env, FLOCK_TEST_ACCOUNT_LIFECYCLE: mode },
    });
    if (response.error) throw response.error;
    assert.equal(response.status, 0, response.stderr);
    return JSON.parse(response.stdout);
  };
  try {
    writeOriginal();
    const switching = { browserRequired: true, switchAccount: true };
    assert.match(invoke("login", "switch-declined", switching).error, /declined/);
    assert.deepEqual(read(), original, "a failed switch keeps the active registration and tokens");
    const switched = invoke("login", "switch-approved", switching).result;
    assert.equal(switched.account, "second@example.com");
    assert.equal(switched.planUsage, true);
    assert.equal(read().clientId, "fixture-new-registration");
    assert.equal(read().hostId, original.hostId);
    for (const [mode, revocationConfirmed] of [["revocation-approved", true], ["revocation-unavailable", false]]) {
      writeOriginal();
      assert.deepEqual(invoke("sign_out", mode).result, { revocationConfirmed });
      assert.deepEqual(read(), { hostId: original.hostId, clientId: original.clientId, subject: original.subject,
        email: original.email, signedOut: true, revocationConfirmed });
      const status = invoke("web_status", mode).result;
      assert.equal(status.connected, false);
      assert.equal(status.planUsage, false);
      assert.equal(status.registered, true);
    }
    writeFileSync(profile, JSON.stringify(signedOutProfile), { mode: 0o600 });
    const prepared = invoke("prepare_login", "same-account", { browserRequired: true });
    assert.equal(prepared.result.account, "first@example.com");
    assert.equal(prepared.candidate.clientId, signedOutProfile.clientId);
    assert.deepEqual(read(), signedOutProfile, "preparing a Web sign-in must leave the profile untouched");
    writeFileSync(profile, JSON.stringify({ hostId: original.hostId, clientId: original.clientId }));
    const unfinished = invoke("web_status", "same-account").result;
    assert.equal(unfinished.connected, false);
    assert.equal(unfinished.registered, true, "an unfinished registration must still allow account switching");
  } finally {
    closeSync(lock);
    rmSync(root, { recursive: true, force: true });
  }
}

async function waitFor(check, message) {
  const deadline = Date.now() + 6000;
  while (Date.now() < deadline) {
    const value = await check();
    if (value) return value;
    await pause(25);
  }
  assert.fail(message);
}

async function checkWebLoginCommit(executable) {
  const root = mkdtempSync(join(tmpdir(), "flock-web-login-"));
  const home = join(root, "home");
  const credentials = join(home, "credentials", "chatgpt");
  const profile = join(credentials, "profile.json");
  let daemon;
  let closed;
  try {
    const env = installNodeWrapper(root);
    mkdirSync(credentials, { recursive: true, mode: 0o700 });
    writeFileSync(profile, JSON.stringify(signedOutProfile), { mode: 0o600 });
    const original = readFileSync(profile, "utf8");
    daemon = childProcess.spawn(executable, ["--home", home, "run", "--port", "0"], {
      env: { ...env, FLOCK_TEST_ACCOUNT_LIFECYCLE: "switch-approved", FLOCK_TEST_ACCOUNT_GATE: root },
      stdio: ["ignore", "pipe", "pipe"],
    });
    closed = once(daemon, "close");
    let output = "", errors = "";
    daemon.stdout.on("data", (data) => { output += data; });
    daemon.stderr.on("data", (data) => { errors += data; });
    const [, url] = await waitFor(() => output.match(/Flock Web UI at (http:\/\/127\.0\.0\.1:\d+)/) ??
      (daemon.exitCode === null ? null : assert.fail(errors)), "Flock did not start");
    const request = async (path, status, body, authorization) => {
      const headers = authorization ? { Authorization: authorization } : {};
      if (body) Object.assign(headers, { Origin: url, "Content-Type": "application/json", "X-Flock-Action": "1" });
      const response = await fetch(url + path, {
        method: body ? "POST" : "GET", headers, body: body ? JSON.stringify(body) : undefined,
        signal: AbortSignal.timeout(3000),
      });
      assert.equal(response.status, status, path);
      return response.json();
    };
    const authorizationFor = ({ person, password }) => "Basic " + Buffer.from(person + ":" + password).toString("base64");
    const access = "/api/settings/installation/access";
    const { credential: owner } = await request(access, 200, { operation: "prepare" });
    await request(access, 200, { operation: "activate", password: owner.password });
    const { credential: delegate } = await request(access, 200,
      { operation: "invite", person: "delegate", adminRoot: true }, authorizationFor(owner));
    const login = { reconsent: false, switchAccount: true };
    await request("/api/settings/model-login", 202, login, authorizationFor(delegate));
    await waitFor(() => existsSync(join(root, "ready")), "OAuth token exchange did not start");
    await request(access, 200, { operation: "role", person: delegate.person, adminRoot: false }, authorizationFor(owner));
    writeFileSync(join(root, "release"), "");
    const stopped = await waitFor(async () => {
      const { model } = await request("/api/settings", 200, undefined, authorizationFor(owner));
      return !model.signingIn && model.signInError && model;
    }, "Sign-in did not reject the demoted administrator");
    assert.match(stopped.signInError, /no longer manage Flock/);
    assert.equal(readFileSync(profile, "utf8"), original, "demotion must preserve the prior profile exactly");

    await request("/api/settings/model-login", 202, login, authorizationFor(owner));
    const connected = await waitFor(async () => {
      const { model } = await request("/api/settings", 200, undefined, authorizationFor(owner));
      return !model.signingIn && model.connected && model;
    }, "The current administrator could not switch accounts after sign-out");
    assert.equal(connected.account, "second@example.com");
    assert.equal(JSON.parse(readFileSync(profile, "utf8")).clientId, "fixture-new-registration");
  } finally {
    if (daemon) {
      daemon.kill();
      await closed;
    }
    rmSync(root, { recursive: true, force: true });
  }
}

if (realpathSync(process.argv[1]) === scriptPath) {
  if (Number(process.versions.node.split(".")[0]) < 22) process.exit(77);
  checkLogin(process.argv[2]);
  checkTransport(process.argv[2]);
  checkAccountLifecycle(process.argv[2]);
  await checkWebLoginCommit(process.argv[2]);
} else if (process.env.FLOCK_TEST_ACCOUNT_LIFECYCLE) {
  simulateAccountLifecycle(process.env.FLOCK_TEST_ACCOUNT_LIFECYCLE);
} else if (process.env.FLOCK_TEST_TRANSPORT_MODE) {
  globalThis.fetch = async (url, options) => {
    const mode = process.env.FLOCK_TEST_TRANSPORT_MODE;
    if (mode !== "scope") return transportResponse(mode);
    assert.equal(url, "https://api.openai.com/v1/responses");
    assert.equal(options.headers.authorization, "Bearer fixture");
    const event = { type: "response.completed", response: {
      output: [{ session: options.headers["session-id"], body: JSON.parse(options.body) }],
    } };
    return new Response(`data: ${JSON.stringify(event)}\n\n`, { headers: { "content-type": "text/event-stream" } });
  };
} else {
  simulateAuthorization(process.env.FLOCK_TEST_LOGIN_MODE);
}
