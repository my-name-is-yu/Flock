import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { environmentWithoutFlock, startFlock } from './daemon.mjs';

const executable = process.argv[2];
const home = mkdtempSync(join(tmpdir(), 'flock-cli-bot-pause-'));
const env = environmentWithoutFlock();
async function withDaemon(verify) {
  const daemon = await startFlock(executable, home, { env });
  try {
    await verify(daemon.origin);
  } finally {
    await daemon.stop();
  }
}
const run = (...args) => spawnSync(executable, ['--home', home, ...args], { env, encoding: 'utf8', timeout: 5000 });


async function setPaused(origin, paused) {
  const response = await fetch(`${origin}/api/bots/pause`, {
    method: 'POST',
    headers: { Origin: origin, 'Content-Type': 'application/json', 'X-Flock-Action': '1' },
    body: JSON.stringify({ botId: 'release', paused }),
    signal: AbortSignal.timeout(5000),
  });
  assert.equal(response.status, 200, await response.text());
}

function checkPaused() {
  const rejected = run('message', 'release', 'This should be refused');
  assert.equal(rejected.status, 1, rejected.stderr);
  assert.equal(rejected.stderr.trim(), 'flock: This Bot is paused.');
}

try {
  const bot = join(home, 'bots', 'release');
  mkdirSync(bot, { recursive: true });
  writeFileSync(join(bot, 'config.json'), JSON.stringify({ name: 'Release' }));

  await withDaemon(async (origin) => {
    await setPaused(origin, true);
    checkPaused();
  });
  checkPaused();
  await withDaemon(async (origin) => {
    checkPaused();
    await setPaused(origin, false);
    const accepted = run('message', 'release', 'This should be accepted');
    assert.equal(accepted.status, 0, accepted.stderr);
    assert.equal(JSON.parse(accepted.stdout).status, 'received');
  });
} finally {
  rmSync(home, { recursive: true, force: true });
}
