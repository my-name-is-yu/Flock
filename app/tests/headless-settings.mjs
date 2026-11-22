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

async function waitForState(id, state) {
  const deadline = Date.now() + 6000;
  let task;
  do {
    task = cli("status", String(id));
    if (task.state === state) return task;
    await setTimeout(50);
  } while (Date.now() < deadline);
  assert.fail(`Expected task ${id} to reach ${state}, got ${JSON.stringify(task)}`);
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

  const daemon = spawn(executable, [...cliArgs, "run", "--fixed", "--no-web"], { env, stdio: ["ignore", "ignore", "inherit"] });
  const stopped = once(daemon, "close");
  try {
    const submitted = cli("submit", "release", "Check the upcoming release");
    const completed = await waitForState(submitted.id, "completed");
    assert.match(completed.lastRun.runId, /^demo-run-/);
    cli("rules", "add", ...launch);
    cli("rules", "add", ...launch);
    const [{kind, action, target, createdAt}, ...other] = cli("rules", "list");
    assert.deepEqual([kind, action, target], launch);
    assert.ok(createdAt > 0);
    assert.deepEqual(other, []);

    const next = cli("submit", "release", "Check again");
    const blocked = await waitForState(next.id, "waiting");
    assert.equal(blocked.wait.reason, "settings");
    assert.match(blocked.wait.text, /denied by installation policy/);
    assert.equal(blocked.lastRun, null);
    assert.deepEqual(cli("rules", "remove", ...launch), []);
    cli("reconcile", ...[blocked.id, blocked.wait.id, blocked.currentRevision, blocked.generation].map(String));
    await waitForState(blocked.id, "completed");
  } finally {
    daemon.kill();
    await stopped;
  }
} finally {
  rmSync(home, { recursive: true, force: true });
}
