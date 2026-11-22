import assert from "node:assert/strict";
import { execFileSync, spawn, spawnSync } from "node:child_process";
import { once } from "node:events";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout } from "node:timers/promises";

const executable = process.argv[2];
const home = mkdtempSync(join(tmpdir(), "flock-headless-access-"));
const cliArgs = ["--home", home];
const options = { encoding: "utf8", timeout: 5000 };
const cli = (...args) => JSON.parse(execFileSync(executable, [...cliArgs, ...args], options));
const launch = ["release", "environment", "demo-env", "launch"];

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

  assert.deepEqual(cli("access", "list", "release"), []);
  const invalid = spawnSync(executable, [...cliArgs, "access", "grant", "release", "environment", "demo-env", "write"], options);
  assert.equal(invalid.status, 1);
  assert.match(invalid.stderr, /Invalid access scope/);
  cli("access", "grant", ...launch);
  const grants = cli("access", "list", "release");
  assert.equal(grants.length, 1);
  const { resourceKind, resourceId, operation, approvedBy } = grants[0];
  assert.deepEqual([resourceKind, resourceId, operation, approvedBy], ["environment", "demo-env", "launch", "local-cli"]);

  const daemon = spawn(executable, [...cliArgs, "run", "--fixed", "--no-web"], { stdio: ["ignore", "ignore", "inherit"] });
  const stopped = once(daemon, "close");
  try {
    const submitted = cli("submit", "release", "Check the upcoming release");
    const completed = await waitForState(submitted.id, "completed");
    assert.match(completed.lastRun.runId, /^demo-run-/);
    cli("access", "revoke", ...launch);
    assert.deepEqual(cli("access", "list", "release"), []);
    const next = cli("submit", "release", "Check again");
    const blocked = await waitForState(next.id, "waiting");
    assert.equal(blocked.wait.reason, "settings");
    assert.match(blocked.wait.text, /Access Settings.*flock access grant/);
    assert.equal(blocked.lastRun, null);
  } finally {
    daemon.kill();
    await stopped;
  }
} finally {
  rmSync(home, { recursive: true, force: true });
}
