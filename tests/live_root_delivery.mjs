import { spawn, spawnSync } from "node:child_process";
import { createHash, randomBytes, randomUUID } from "node:crypto";
import { once } from "node:events";
import { createReadStream } from "node:fs";
import { mkdir, mkdtemp, open } from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { DatabaseSync } from "node:sqlite";
import { setTimeout as delay } from "node:timers/promises";
import assert from "node:assert/strict";

const executable = process.env.T3_EXACT_SERVER_EXECUTABLE;
const entry = process.env.T3_EXACT_SERVER_ENTRY;
assert.ok(Boolean(executable) !== Boolean(entry), "Choose a pinned T3 executable or server entry");
const artifact = resolve(executable ?? entry);
const flockExecutable = resolve(
  process.env.FLOCK_LIVE_ROOT_DELIVERY ?? join(import.meta.dirname, "..", "build", "flock_live_root_delivery"),
);
const withInvitation = process.argv.includes("--invitation");
const modelSelection = { instanceId: "codex", model: "gpt-6-luna" };
const host = "127.0.0.1";
const listener = createServer();
listener.listen(0, host);
await once(listener, "listening");
const { port } = listener.address();
await new Promise((resolve) => listener.close(resolve));
const origin = `http://${host}:${port}`;
const base = await mkdtemp(join(tmpdir(), "flock-root-delivery-live-"));
const root = join(base, "project");
const fixtureStoreDir = join(base, "flock");
await mkdir(root, { mode: 0o700 });
await mkdir(fixtureStoreDir, { mode: 0o700 });

const bootstrap = randomBytes(32).toString("hex");
const credential = randomBytes(32).toString("base64url");
const log = await open(join(base, "server.log"), "w", 0o600);
const server = spawn(
  executable ? artifact : process.execPath,
  [
    ...(executable ? [] : [artifact]),
    "serve", "--mode", "desktop", "--base-dir", base,
    "--host", host, "--port", String(port), "--bootstrap-fd", "3",
  ],
  {
    cwd: base,
    stdio: ["ignore", log.fd, log.fd, "pipe"],
    env: { ...process.env, T3CODE_HOME: base, T3CODE_OTEL_SDK_DISABLED: "true" },
  },
);
let spawnError;
server.once("error", (error) => { spawnError = error; });
server.stdio[3].on("error", () => {});
server.stdio[3].end(JSON.stringify({
  mode: "desktop", noBrowser: true, port, t3Home: base, host, desktopBootstrapToken: bootstrap,
  shellEnvironmentPrepared: true, tailscaleServeEnabled: false, tailscaleServePort: 443,
}) + "\n");

async function sha256(path) {
  const hash = createHash("sha256");
  for await (const bytes of createReadStream(path)) hash.update(bytes);
  return hash.digest("hex");
}

async function until(label, action, timeout = 20_000) {
  const expiresAt = Date.now() + timeout;
  while (Date.now() < expiresAt) {
    if (spawnError || server.exitCode !== null || server.signalCode !== null) {
      throw new Error(`Isolated T3 did not stay available. See ${join(base, "server.log")}`);
    }
    const result = await action();
    if (result) return result;
    await delay(100);
  }
  throw new Error(`Timed out waiting for ${label}. See ${base}`);
}

function runFlock(operation, fixture, ref = {}) {
  const child = spawnSync(flockExecutable, [], {
    input: JSON.stringify({ operation, ...fixture, ...ref }),
    encoding: "utf8",
    timeout: 45_000,
    maxBuffer: 64 * 1024,
  });
  if (child.error || child.status !== 0) {
    throw new Error(`Flock ${operation} failed: ${child.stderr?.trim() || child.error?.code || child.status}`);
  }
  return JSON.parse(child.stdout);
}

let ws;
let db;
let flockDb;
try {
  const descriptor = await until("T3 environment", async () => {
    const response = await fetch(`${origin}/.well-known/t3/environment`, {
      signal: AbortSignal.timeout(1000),
    }).catch(() => undefined);
    return response?.ok ? response.json() : undefined;
  });
  assert.match(descriptor.serverVersion, /^0\.1\.11(?:[-+].+)?$/);
  assert.ok(descriptor.databaseMigration >= 103, "The pinned candidate lacks exact-run storage");
  const authentication = await fetch(`${origin}/api/auth/browser-session`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ credential: bootstrap }),
  });
  assert.equal(authentication.status, 200, "Human bootstrap auth");
  const cookie = authentication.headers.getSetCookie()[0].split(";", 1)[0];
  const socketAuth = await fetch(`${origin}/api/auth/websocket-ticket`, {
    method: "POST",
    headers: { cookie },
  });
  assert.equal(socketAuth.status, 200, "Human WebSocket ticket");
  const ticket = (await socketAuth.json()).ticket;
  ws = new WebSocket(`ws://${host}:${port}/ws?orchestrationProtocol=2&wsTicket=${encodeURIComponent(ticket)}`);
  await new Promise((resolve, reject) => {
    ws.addEventListener("open", resolve, { once: true });
    ws.addEventListener("error", () => reject(new Error("Could not open fixture WebSocket")), { once: true });
  });

  let sequence = 0;
  function rpc(tag, payload) {
    const id = String(++sequence);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        ws.removeEventListener("message", receive);
        reject(new Error(`Timed out on ${tag}`));
      }, 15_000);
      const receive = (event) => {
        const decoded = JSON.parse(event.data);
        for (const frame of Array.isArray(decoded) ? decoded : [decoded]) {
          if (frame._tag !== "Exit" || frame.requestId !== id) continue;
          clearTimeout(timer);
          ws.removeEventListener("message", receive);
          if (frame.exit._tag === "Success") resolve(frame.exit.value);
          else reject(new Error(`T3 refused ${tag}. See ${join(base, "server.log")}`));
        }
      };
      ws.addEventListener("message", receive);
      ws.send(JSON.stringify({ _tag: "Request", id, tag, payload, headers: [] }));
    });
  }

  const projectId = randomUUID();
  const threadId = randomUUID();
  const sourceMessageId = randomUUID();
  await rpc("projects.mutate", {
    type: "project.create", commandId: randomUUID(), projectId, title: "Flock root delivery fixture",
    workspaceRoot: root, defaultModelSelection: modelSelection,
  });
  await rpc("orchestration.launchThread", {
    commandId: randomUUID(), threadId, projectId, title: "Flock delivery provider check", generateTitle: false,
    modelSelection, runtimeMode: "auto", interactionMode: "default", workspaceStrategy: { type: "root" },
    initialMessage: {
      messageId: sourceMessageId,
      attachments: [],
      text: "This is a local integration test. As your first tool call, run /bin/sleep 16 in the current directory. Do not modify files or run any other command. After the command finishes, acknowledge the most recent instruction.",
    },
  });
  db = new DatabaseSync(join(base, "userdata", "statev2.sqlite"), { readOnly: true });
  const run = await until("running Codex provider turn", () => {
    const row = db.prepare(`
      SELECT r.run_id, r.status, json_extract(pt.payload_json, '$.nativeTurnRef.nativeId') AS native_turn_id
      FROM orchestration_v2_projection_runs r
      LEFT JOIN orchestration_v2_projection_run_attempts a ON a.attempt_id = json_extract(r.payload_json, '$.activeAttemptId')
      LEFT JOIN orchestration_v2_projection_provider_turns pt ON pt.run_attempt_id = a.attempt_id
        AND pt.node_id = a.root_node_id AND pt.status='running'
      WHERE r.thread_id = ?
    `).get(threadId);
    if (row && ["completed", "failed", "interrupted", "cancelled"].includes(row.status)) {
      throw new Error(`Original Run ended before admission with ${row.status}. See ${base}`);
    }
    return row?.status === "running" && row.native_turn_id ? row : undefined;
  }, 60_000);
  const target = { environmentId: descriptor.environmentId, projectId, threadId, runId: run.run_id };
  const grantId = randomUUID();
  const botId = "verification-bot";

  async function humanPost(path, body) {
    const response = await fetch(`${origin}/api/orchestration/exact-run/${path}`, {
      method: "POST",
      headers: { "content-type": "application/json", cookie },
      body: JSON.stringify(body),
      signal: AbortSignal.timeout(15_000),
    });
    const data = await response.json();
    assert.equal(response.status, 200, `${path}: ${data.reason ?? "unexpected HTTP status"}`);
    return data;
  }
  const permission = {
    target,
    sourceMessageId,
    botId,
    permission: { kind: "consult" },
    credentialDigest: createHash("sha256").update(credential).digest("hex"),
  };
  let personRef;
  if (withInvitation) {
    const created = await humanPost("invitations/create", {
      ...permission, requestId: grantId, contextItemIds: [sourceMessageId],
    });
    assert.ok(created.personRef);
    personRef = created.personRef;
  } else {
    const grant = await humanPost("grants/open", { ...permission, grantId });
    assert.equal(grant.epoch, 0);
    assert.deepEqual(grant.target, target);
  }
  const fixture = { fixtureStoreDir, t3Origin: origin, target, grantId, sourceMessageId, botId, credential, personRef };
  const acceptedCount = () => db.prepare(
    "SELECT count(*) AS count FROM exact_run_receipts WHERE grant_id = ?",
  ).get(grantId).count;
  const prepared = runFlock("prepare", fixture);
  assert.equal(prepared.state, "prepared");
  assert.equal(acceptedCount(), 0, "Preparation admitted external input");
  const ref = { channelId: prepared.channelId, commandId: prepared.commandId };
  const applied = runFlock("deliver", fixture, ref);
  assert.equal(applied.state, "applied");
  assert.equal(applied.commandId, ref.commandId);
  assert.equal(applied.runId, run.run_id);
  assert.equal(applied.nativeTurnId, run.native_turn_id);
  assert.equal(applied.serverVersion, descriptor.serverVersion);
  assert.ok(applied.nativeItemId);
  assert.equal(acceptedCount(), 1);

  const seal = await humanPost("grants/seal", { target, grantId, epoch: 0 });
  assert.ok(seal.grant.sealed);
  assert.deepEqual(seal.residualCommandIds, [ref.commandId]);
  const recovered = runFlock("recover", fixture, ref);
  assert.deepEqual(recovered, { ...applied, sealed: true });

  const rootState = await until("normal root completion", () => {
    const row = db.prepare("SELECT status FROM orchestration_v2_projection_runs WHERE run_id = ?").get(run.run_id);
    if (["failed", "interrupted", "cancelled"].includes(row?.status)) {
      throw new Error(`Original Run ended with ${row.status}. See ${base}`);
    }
    return row?.status === "completed" ? row.status : undefined;
  }, 45_000);
  const newRuns = db.prepare(
    "SELECT count(*) AS count FROM orchestration_v2_projection_runs WHERE thread_id = ? AND run_id <> ?",
  ).get(threadId, run.run_id).count;
  const interruptRequests = db.prepare(
    "SELECT count(*) AS count FROM orchestration_v2_projection_turn_items WHERE run_id = ? AND type = 'run_interrupt_request'",
  ).get(run.run_id).count;
  assert.equal(newRuns, 0);
  assert.equal(interruptRequests, 0);
  assert.equal(acceptedCount(), 1, "Duplicate or recovery admitted another command");
  const providerObservation = db.prepare(
    "SELECT observation_json FROM exact_run_receipts WHERE grant_id = ? AND command_id = ?",
  ).get(grantId, ref.commandId);
  assert.deepEqual(JSON.parse(providerObservation.observation_json), {
    state: "applied", nativeTurnId: applied.nativeTurnId, nativeItemId: applied.nativeItemId,
  });
  flockDb = new DatabaseSync(join(fixtureStoreDir, "root-delivery.sqlite3"), { readOnly: true });
  const local = flockDb.prepare("SELECT count(*) AS count, sum(send_attempts) AS attempts FROM root_deliveries").get();
  assert.equal(local.count, 1);
  assert.equal(local.attempts, 1);

  const codexVersion = spawnSync("codex", ["--version"], { encoding: "utf8" });
  assert.equal(codexVersion.status, 0, "Could not record the Codex version");
  console.log(JSON.stringify({
    serverVersion: descriptor.serverVersion,
    databaseMigration: descriptor.databaseMigration,
    t3LaunchArtifact: { kind: executable ? "native-executable" : "bundle-entry", sha256: await sha256(artifact) },
    flockExecutableSha256: await sha256(flockExecutable),
    codexVersion: codexVersion.stdout.trim(),
    model: modelSelection.model,
    target, grantId, commandId: ref.commandId, botId,
    providerEvidence: JSON.parse(providerObservation.observation_json),
    finalGrantSeal: recovered.sealed,
    sameReceiptAfterSeal: true,
    acceptedReceipts: acceptedCount(),
    localSendAttempts: local.attempts,
    newRuns, interruptRequests, rootState,
    ingress: withInvitation ? "authenticated-session-invitation" : "authenticated-grant-fixture",
    evidenceDir: base,
  }, null, 2));
} finally {
  ws?.close();
  flockDb?.close();
  db?.close();
  if (!spawnError && server.exitCode === null && server.signalCode === null) {
    const exited = once(server, "exit");
    server.kill("SIGTERM");
    await exited;
  }
  await log.close();
}
