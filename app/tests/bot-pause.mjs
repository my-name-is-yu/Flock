import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const executable = process.argv[2];
const home = mkdtempSync(join(tmpdir(), 'flock-cli-bot-pause-'));
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith('FLOCK_')));
const run = (...args) => spawnSync(executable, ['--home', home, ...args], { env, encoding: 'utf8', timeout: 5000 });

async function withDaemon(verify) {
  const child = spawn(executable, ['--home', home, 'run', '--fixed', '--port', '0'],
    { env, stdio: ['ignore', 'pipe', 'inherit'] });
  const closed = once(child, 'close');
  const started = new Promise((resolve, reject) => {
    let stdout = '';
    child.stdout.setEncoding('utf8').on('data', (text) => {
      stdout += text;
      const origin = stdout.match(/Flock Web UI at (http:\/\/127\.0\.0\.1:\d+)/)?.[1];
      if (origin) resolve(origin);
    });
    child.once('error', reject);
    child.once('close', (code) => reject(new Error(`Flock exited during startup (${code})`)));
  });
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error('Flock did not start')), 5000);
  });
  try {
    await verify(await Promise.race([started, timeout]));
  } finally {
    clearTimeout(timer);
    child.kill();
    await closed;
  }
}

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
