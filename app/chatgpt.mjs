import { spawn } from "node:child_process";
import { createHash, createPublicKey, randomBytes, randomUUID, timingSafeEqual, verify } from "node:crypto";
import { closeSync, constants, fstatSync, fsyncSync, openSync, readFileSync, renameSync, unlinkSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { join } from "node:path";

const issuer = "https://auth.openai.com";
const resource = "https://api.openai.com/v1";
const directScope = "chatgpt.tokens.use.direct";
const scopes = `openid profile email offline_access resource.invoke ${directScope}`;
const callbackPath = "/auth/callback";
const maxResponse = 4 * 1024 * 1024;
const maxRequest = 8 * 1024 * 1024;
const random = () => randomBytes(32).toString("base64url");
const [operation, directory, inheritedLock] = process.argv.slice(2);
const lockDescriptor = Number(inheritedLock);
const profilePath = join(directory ?? ".", "profile.json");
let locked = Number.isSafeInteger(lockDescriptor) && lockDescriptor > 2;

class Failure extends Error {
  constructor(message, transient = false, code = "") {
    super(message);
    this.transient = transient;
    this.code = code;
  }
}

function unlock() {
  if (locked) {
    locked = false;
    closeSync(lockDescriptor);
  }
}

function readProfile() {
  try {
    const fd = openSync(profilePath, constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK);
    try {
      const info = fstatSync(fd);
      if (!info.isFile() || info.uid !== process.getuid() || (info.mode & 0o077) || info.size > maxResponse) {
        throw new Failure("ChatGPT credential file must be a private regular file");
      }
      return JSON.parse(readFileSync(fd, "utf8"));
    } finally {
      closeSync(fd);
    }
  } catch (error) {
    if (error.code === "ENOENT") return {};
    if (error instanceof Failure) throw error;
    throw new Failure("Unable to read Flock's ChatGPT credentials");
  }
}

function saveProfile(profile) {
  const temporary = join(directory, `.profile-${randomUUID()}`);
  const fd = openSync(temporary, "wx", 0o600);
  try {
    try {
      writeFileSync(fd, JSON.stringify(profile));
      fsyncSync(fd);
    } finally {
      closeSync(fd);
    }
    renameSync(temporary, profilePath);
  } finally {
    try { unlinkSync(temporary); } catch {}
  }
}

function safeCode(value) {
  return typeof value === "string" && /^[a-z0-9_.:-]{1,100}$/i.test(value) ? value : "";
}

function remoteFailure(site, status, value, requestId) {
  const error = value?.error;
  const code = safeCode(typeof error === "string" ? error : error?.code ?? value?.code);
  const parameter = safeCode(error?.param ?? value?.param);
  const tag = [status ? `HTTP ${status}` : "", code, parameter ? `parameter ${parameter}` : "",
    safeCode(requestId) ? `request ${requestId}` : ""].filter(Boolean).join(", ");
  const temporary = (status === 429 || status === 408 || status >= 500 || code.endsWith("_unavailable"))
    && code !== "subscription_sharing_usage_limit_exceeded";
  const guidance = code === "subscription_sharing_usage_limit_exceeded"
    ? " — review the app limit at https://chatgpt.com/settings/usage"
    : site !== "ChatGPT sign-in" && status === 401
      ? " — check Flock's ChatGPT connection and granted plan permissions"
      : "";
  return new Failure(`${site} rejected the request${tag ? " (" + tag + ")" : ""}${guidance}`, temporary, code);
}

async function request(url, options = {}, timeout = 15_000) {
  try {
    return await fetch(url, { ...options, redirect: "error", signal: AbortSignal.timeout(timeout) });
  } catch {
    throw new Failure("Unable to reach ChatGPT within the request deadline", true);
  }
}

async function jsonResponse(response) {
  try {
    return await response.json();
  } catch {
    return null;
  }
}

async function discover() {
  const response = await request(`${issuer}/.well-known/openid-configuration`);
  const data = await jsonResponse(response);
  if (!response.ok) throw remoteFailure("ChatGPT sign-in", response.status, data);
  if (data?.issuer !== issuer || ["authorization_endpoint", "token_endpoint", "jwks_uri"].some((key) => {
    try { return new URL(data[key]).origin !== issuer; } catch { return true; }
  })) throw new Failure("Could not verify the OpenAI sign-in discovery document");
  return data;
}

async function identity(idToken, clientId, discovery, nonce, now = Date.now()) {
  let parts, header, claims;
  try {
    parts = idToken.split(".");
    if (parts.length !== 3 || parts.some((part) => !/^[\w-]+$/.test(part))) throw new Error();
    header = JSON.parse(Buffer.from(parts[0], "base64url"));
    claims = JSON.parse(Buffer.from(parts[1], "base64url"));
  } catch {
    throw new Failure("Invalid ChatGPT ID token", false, "invalid_id_token");
  }
  const audience = Array.isArray(claims?.aud) ? claims.aud : [claims?.aud];
  if (header?.alg !== "RS256" || !header.kid || claims?.iss !== issuer || !audience.includes(clientId) ||
      (claims.azp !== undefined && claims.azp !== clientId) || (audience.length > 1 && claims.azp !== clientId) ||
      typeof claims.sub !== "string" || !claims.sub || !Number.isFinite(claims.exp) || claims.exp * 1000 <= now ||
      !Number.isFinite(claims.iat) || claims.iat * 1000 > now + 5000 ||
      (claims.nbf !== undefined && (!Number.isFinite(claims.nbf) || claims.nbf * 1000 > now + 5000)) ||
      (nonce !== undefined && nonce !== claims.nonce)) {
    throw new Failure("ChatGPT ID token did not match this authorization", false, "invalid_id_token");
  }
  const response = await request(discovery.jwks_uri);
  const data = await jsonResponse(response);
  if (!response.ok || !Array.isArray(data?.keys)) throw new Failure("OpenAI verification keys are unavailable", true);
  const key = data.keys.find((item) => item.kid === header.kid && item.kty === "RSA" &&
    (item.use === undefined || item.use === "sig") && (item.alg === undefined || item.alg === "RS256"));
  if (!key) throw new Failure("OpenAI verification key is unavailable", true);
  let valid = false;
  try {
    valid = verify("RSA-SHA256", Buffer.from(`${parts[0]}.${parts[1]}`),
      createPublicKey({ key, format: "jwk" }), Buffer.from(parts[2], "base64url"));
  } catch {}
  if (!valid) throw new Failure("ChatGPT ID token signature could not be verified", false, "invalid_id_token");
  return { subject: claims.sub, email: typeof claims.email === "string" ? claims.email : "" };
}

async function exchange(discovery, parameters) {
  const response = await request(discovery.token_endpoint, {
    method: "POST",
    headers: { accept: "application/json", "content-type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({ ...parameters, resource }),
  });
  const tokens = await jsonResponse(response);
  if (!response.ok) throw remoteFailure("ChatGPT sign-in", response.status, tokens, response.headers.get("x-request-id"));
  if (!tokens || typeof tokens !== "object") throw new Failure("Invalid ChatGPT token response");
  return tokens;
}

function credentials(tokens, previous) {
  const granted = tokens.scope ?? previous?.granted?.join(" ");
  if (typeof granted !== "string") throw new Failure("ChatGPT did not confirm the granted permissions");
  const next = { granted: granted.split(/\s+/).filter(Boolean) };
  if (next.granted.includes(directScope)) {
    if (typeof tokens.access_token !== "string" || !/^[\x21-\x7e]+$/.test(tokens.access_token) ||
        typeof tokens.refresh_token !== "string" || !tokens.refresh_token ||
        typeof tokens.token_type !== "string" || tokens.token_type.toLowerCase() !== "bearer" ||
        !Number.isFinite(Date.now() + tokens.expires_in * 1000) || typeof tokens.expires_in !== "number" || tokens.expires_in <= 0) {
      throw new Failure("ChatGPT returned incomplete plan credentials");
    }
    next.accessToken = tokens.access_token;
    next.refreshToken = tokens.refresh_token;
    next.expiresAt = Date.now() + tokens.expires_in * 1000;
    const earliest = tokens.earliest_refresh_at;
    const earliestMs = typeof earliest === "number" ? earliest * 1000 : Date.parse(earliest);
    if (Number.isFinite(earliestMs)) next.earliestRefreshAt = earliestMs;
  }
  return next;
}

async function listen(state, savedClient) {
  let done, fail;
  const result = new Promise((resolve, reject) => { done = resolve; fail = reject; });
  let origin;
  const server = createServer({ requestTimeout: 15_000, headersTimeout: 10_000 }, (req, res) => {
    res.setHeader("Cache-Control", "no-store");
    res.setHeader("Referrer-Policy", "no-referrer");
    res.setHeader("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
    let url;
    try { url = new URL(req.url, origin); } catch { res.writeHead(400).end(); return; }
    if (req.method !== "GET" || req.headers.host !== new URL(origin).host || url.origin !== origin) {
      res.writeHead(400).end();
      return;
    }
    if (url.pathname === "/auth/complete") {
      res.setHeader("Content-Type", "text/plain; charset=utf-8");
      res.end("Return to Flock to check your ChatGPT connection.");
      return;
    }
    if (url.pathname !== callbackPath) { res.writeHead(404).end(); return; }
    const parameters = url.searchParams;
    const actual = Buffer.from(parameters.get("state") ?? "");
    const expected = Buffer.from(state);
    if (parameters.getAll("state").length !== 1 || actual.length !== expected.length || !timingSafeEqual(actual, expected)) {
      res.writeHead(400).end("Invalid sign-in state.");
      return;
    }
    res.writeHead(303, { Location: "/auth/complete" }).end();
    if (parameters.has("error")) { fail(new Failure("ChatGPT authorization was declined or expired")); return; }
    const code = parameters.get("code");
    const clientId = parameters.get("client_id") ?? savedClient;
    if (parameters.getAll("code").length !== 1 || !code || parameters.getAll("client_id").length > 1 ||
        !clientId || !/^[\w-]{1,200}$/.test(clientId) || clientId === "dynamic_agent_client" ||
        (savedClient && clientId !== savedClient)) {
      fail(new Failure("ChatGPT did not return a valid Flock client registration"));
      return;
    }
    done({ code, clientId });
  });
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  server.on("error", fail);
  origin = `http://127.0.0.1:${server.address().port}`;
  const timer = setTimeout(() => fail(new Failure("ChatGPT browser approval timed out")), 300_000);
  return {
    result, redirectUri: origin + callbackPath,
    close() { clearTimeout(timer); server.close(); server.closeAllConnections(); },
  };
}

function openBrowser(url) {
  return new Promise((resolve) => {
    const browser = spawn(process.platform === "darwin" ? "/usr/bin/open" : "xdg-open", [url], { stdio: "ignore" });
    browser.once("error", () => resolve(false));
    browser.once("exit", (code) => resolve(code === 0));
  });
}

async function showBrowserOrPrintUrl(url, printUrl, callbackSettled) {
  if (printUrl || (!await openBrowser(url) && !callbackSettled())) console.error(url);
}

async function login({ printUrl = false, reconsent = false } = {}) {
  let profile = readProfile();
  if (!profile.hostId) {
    profile = { hostId: `urn:uuid:${randomUUID()}` };
    saveProfile(profile);
  }
  const discovery = await discover();
  const state = random(), nonce = random(), verifier = random();
  const listener = await listen(state, profile.clientId);
  try {
    const url = new URL(discovery.authorization_endpoint);
    url.search = new URLSearchParams({
      client_id: profile.clientId ?? "dynamic_agent_client",
      ext_agent_host_id: profile.hostId,
      response_type: "code",
      redirect_uri: listener.redirectUri,
      scope: scopes,
      resource,
      state,
      nonce,
      code_challenge_method: "S256",
      code_challenge: createHash("sha256").update(verifier).digest("base64url"),
    }).toString();
    if (!profile.clientId) url.searchParams.set("agent_name_hint", "Flock");
    if (reconsent && profile.clientId) url.searchParams.set("prompt", "consent");
    console.error("Continue with ChatGPT. Approve Flock's plan access in your browser.");
    let callbackSettled = false;
    const callback = listener.result.then(
      result => { callbackSettled = true; return result; },
      error => { callbackSettled = true; throw error; },
    );
    const [{ code, clientId }] = await Promise.all([
      callback, showBrowserOrPrintUrl(url.toString(), printUrl, () => callbackSettled),
    ]);
    if (!profile.clientId) {
      profile.clientId = clientId;
      saveProfile(profile);
    }
    const tokens = await exchange(discovery, {
      grant_type: "authorization_code", client_id: clientId, code, code_verifier: verifier,
      redirect_uri: listener.redirectUri,
    });
    const account = await identity(tokens.id_token, clientId, discovery, nonce);
    if (profile.subject && account.subject !== profile.subject) {
      throw new Failure("Sign in to the ChatGPT account already registered with this Flock home");
    }
    const next = { hostId: profile.hostId, clientId, ...account, idToken: tokens.id_token, ...credentials(tokens) };
    saveProfile(next);
    return status(next);
  } finally {
    listener.close();
  }
}

function status(profile) {
  return {
    connected: Boolean(profile.subject),
    account: profile.email ?? "",
    planUsage: Boolean(profile.refreshToken && profile.granted?.includes(directScope)),
    manageUsage: "https://chatgpt.com/settings/usage",
  };
}

async function accessToken() {
  let profile = readProfile();
  if (!status(profile).planUsage) throw new Failure("Use 'flock model login' and grant ChatGPT plan usage");
  const now = Date.now();
  if (profile.pending || (profile.expiresAt <= now + 60_000 && (profile.earliestRefreshAt ?? 0) <= now)) {
    const discovery = await discover();
    let pending = profile.pending;
    try {
      if (!pending) {
        const tokens = await exchange(discovery, {
          grant_type: "refresh_token", client_id: profile.clientId, refresh_token: profile.refreshToken,
        });
        pending = { ...credentials(tokens, profile), idToken: tokens.id_token, receivedAt: Date.now() };
        // Preserve the rotated token before doing network I/O for ID verification.
        saveProfile({ ...profile, pending });
      }
      const account = pending.idToken === undefined ? {} :
        await identity(pending.idToken, profile.clientId, discovery, undefined, pending.receivedAt);
      if (account.subject && account.subject !== profile.subject) {
        throw new Failure("Refreshed ChatGPT identity does not match the Flock profile", false, "invalid_id_token");
      }
      const { receivedAt, idToken, ...next } = pending;
      profile = {
        hostId: profile.hostId, clientId: profile.clientId, subject: profile.subject, email: account.email ?? profile.email,
        idToken: idToken ?? profile.idToken, ...next,
      };
      saveProfile(profile);
    } catch (error) {
      if (["invalid_grant", "invalid_refresh_token", "token_expired", "refresh_token_expired",
        "refresh_token_invalidated", "refresh_token_reused", "invalid_id_token"].includes(error.code)) {
        saveProfile({ hostId: profile.hostId, clientId: profile.clientId, subject: profile.subject, email: profile.email });
        throw new Failure("Flock's ChatGPT connection expired. Run 'flock model login' again");
      }
      throw error;
    }
  }
  if (!status(profile).planUsage) {
    throw new Failure("Flock's ChatGPT plan permission is unavailable. Run 'flock model login --reconsent'");
  }
  if (profile.expiresAt <= Date.now()) {
    throw new Failure("ChatGPT credentials cannot be renewed yet. Retry shortly.", true);
  }
  unlock();
  return profile.accessToken;
}

async function api(path, token, options, timeout) {
  const response = await request(resource + path, {
    ...options, headers: { ...options?.headers, authorization: `Bearer ${token}` },
  }, timeout);
  if (!response.ok) throw remoteFailure("ChatGPT Responses API", response.status, await jsonResponse(response),
    response.headers.get("x-request-id"));
  return response;
}

async function models() {
  const response = await api("/models", await accessToken());
  const data = await jsonResponse(response);
  if (!Array.isArray(data?.models)) throw new Failure("ChatGPT did not return a model catalog", true);
  return data.models.filter((model) => model.visibility === "list" && typeof model.slug === "string")
    .map(({ slug, display_name }) => ({ slug, displayName: display_name ?? slug }));
}

async function parseStream(body, requestId) {
  let text = "", data = [], received = 0, completed;
  function line(value) {
    if (value.endsWith("\r")) value = value.slice(0, -1);
    if (value.startsWith("data:")) data.push(value.slice(5).replace(/^ /, ""));
    if (value || !data.length) return;
    const payload = data.join("\n");
    data = [];
    if (payload === "[DONE]") return;
    const event = JSON.parse(payload);
    if (event.type === "response.completed") completed = event.response;
    if (event.type === "response.incomplete" || event.type === "response.failed" || event.type === "error") {
      const failure = event.type === "response.incomplete"
        ? new Failure("ChatGPT returned an incomplete response", true)
        : remoteFailure("ChatGPT Responses API", 0, event.response ?? event, requestId);
      failure.usage = event.response?.usage ?? event.usage;
      throw failure;
    }
  }
  try {
    for await (const chunk of body.pipeThrough(new TextDecoderStream())) {
      received += chunk.length;
      if (received > maxResponse) throw new Failure("ChatGPT response exceeded the transport limit");
      text += chunk;
      let end;
      while ((end = text.indexOf("\n")) !== -1) {
        line(text.slice(0, end));
        text = text.slice(end + 1);
        if (completed) return completed;
      }
    }
  } catch (error) {
    if (error instanceof Failure) throw error;
    throw new Failure("ChatGPT response stream was interrupted or malformed", true);
  }
  throw new Failure("ChatGPT stream ended before a completed response", true);
}

async function infer(payload) {
  const response = await api("/responses", await accessToken(), {
    method: "POST",
    headers: { "content-type": "application/json", accept: "text/event-stream" },
    body: JSON.stringify(payload),
  }, 90_000);
  const mediaType = response.headers.get("content-type")?.split(";")[0]?.trim().toLowerCase();
  if (!response.body || (mediaType && mediaType !== "text/event-stream")) {
    throw new Failure("ChatGPT did not return the expected response stream", true);
  }
  const completed = await parseStream(response.body, response.headers.get("x-request-id"));
  return { output: completed.output, usage: completed.usage };
}

try {
  if (!locked) throw new Failure("Flock's ChatGPT transport requires an inherited credential lock");
  let payload = "";
  let requestBytes = 0;
  for await (const chunk of process.stdin.setEncoding("utf8")) {
    requestBytes += Buffer.byteLength(chunk);
    if (requestBytes > maxRequest) throw new Failure("Flock's ChatGPT request exceeded the transport limit");
    payload += chunk;
  }
  const input = JSON.parse(payload);
  let result;
  if (operation === "login") result = await login(input);
  else if (operation === "status") result = status(readProfile());
  else if (operation === "models") result = await models();
  else if (operation === "infer") result = await infer(input);
  else throw new Failure("Unknown ChatGPT transport operation");
  console.log(JSON.stringify({ result }));
} catch (error) {
  const known = error instanceof Failure;
  // Raw browser, filesystem, fetch and parse errors can contain credentials or prompts.
  console.log(JSON.stringify({
    error: known ? error.message : "Flock's ChatGPT transport failed",
    transient: known && error.transient,
    usage: known ? error.usage : undefined,
  }));
} finally {
  unlock();
}
