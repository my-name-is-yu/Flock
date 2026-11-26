import assert from "node:assert/strict";
import { execFileSync, spawn, spawnSync } from "node:child_process";
import { once } from "node:events";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout } from "node:timers/promises";

const executable = process.argv[2];
const home = mkdtempSync(join(tmpdir(), "flock-headless-settings-"));
const cliArgs = ["--home", home];
// Keep the fixture independent of the developer's Flock configuration.
const env = Object.fromEntries(
  Object.entries(process.env).filter(([name]) => !name.startsWith("FLOCK_")),
);
const options = { encoding: "utf8", timeout: 5000, env };
const cli = (...args) => JSON.parse(execFileSync(executable, [...cliArgs, ...args], options));
const launch = ["never", "environment.launch", "demo-*"];

async function waitForState(messageId, state) {
  const deadline = Date.now() + 10000;
  let task;
  do {
    task = cli("tasks", "release").find((item) => item.sourceMessageId === messageId);
    if (task?.state === state) return task;
    await setTimeout(50);
  } while (Date.now() < deadline);
  assert.fail(`Expected the task from message ${messageId} to reach ${state}, got ${JSON.stringify(task)}`);
}

try {
  const bot = join(home, "bots", "release");
  mkdirSync(bot, { recursive: true });
  writeFileSync(join(bot, "config.json"), JSON.stringify({
    modelProvider: "chatgpt",
    model: "demo",
    environmentId: "demo-env",
    projectId: "demo-project",
  }));

  assert.deepEqual(cli("rules", "list"), []);
  const invalid = spawnSync(executable, [...cliArgs, "rules", "add", "never", "environment.write", "demo-env"], options);
  assert.equal(invalid.status, 1);
  assert.match(invalid.stderr, /Unknown policy action/);

  const removed = spawnSync(executable, [...cliArgs, "submit", "release", "Check the release"], options);
  assert.equal(removed.status, 1);
  assert.match(removed.stderr, /Unknown command: submit/);

  const missingBot = spawnSync(executable, [...cliArgs, "message", "missing", "Hello"], options);
  assert.equal(missingBot.status, 1);
  assert.match(missingBot.stderr, /Bot does not exist: missing/);

  const draftBot = join(home, "bots", "draft");
  mkdirSync(draftBot, { recursive: true });
  writeFileSync(join(draftBot, "config.json"), JSON.stringify({ name: "Draft" }));
  const draft = cli("message", "draft", "Hello");
  assert.equal(draft.status, "received");
  assert.ok(draft.messageId > 0);

  const received = cli("message", "release", "Check the upcoming release");
  assert.equal(received.status, "received");
  assert.ok(received.messageId > 0);
  assert.deepEqual(cli("tasks"), []);

  const daemon = spawn(executable, [...cliArgs, "run", "--fixed", "--no-web"], { env, stdio: ["ignore", "ignore", "inherit"] });
  const stopped = once(daemon, "close");
  try {
    const completed = await waitForState(received.messageId, "completed");
    assert.match(completed.lastRun.runId, /^demo-run-/);
    cli("rules", "add", ...launch);
    cli("rules", "add", ...launch);
    const [{kind, action, target, createdAt}, ...other] = cli("rules", "list");
    assert.deepEqual([kind, action, target], launch);
    assert.ok(createdAt > 0);
    assert.deepEqual(other, []);

    const next = cli("message", "release", "Check again");
    const blocked = await waitForState(next.messageId, "waiting");
    assert.equal(blocked.wait.reason, "settings");
    assert.match(blocked.wait.text, /denied by installation policy/);
    assert.equal(blocked.lastRun, null);
    assert.deepEqual(cli("rules", "remove", ...launch), []);
    await waitForState(next.messageId, "completed");
  } finally {
    daemon.kill();
    await stopped;
  }
} finally {
  rmSync(home, { recursive: true, force: true });
}
