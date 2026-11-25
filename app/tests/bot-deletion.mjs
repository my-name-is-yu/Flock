import assert from 'node:assert/strict';
import { execFileSync, spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

const [, , executable, python] = process.argv;
const home = mkdtempSync(join(tmpdir(), 'flock-bot-deletion-'));
const cliArgs = ['--home', home];
const identities = join(home, 'identities.json');
const root = 'Bearer fixture-deletion-root';
const owner = 'Bearer fixture-deletion-owner';
const member = 'Bearer fixture-deletion-member';
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith('FLOCK_')));
env.FLOCK_WEB_IDENTITIES_FILE = identities;
const cliOptions = { encoding: 'utf8', timeout: 5000, env };
const cli = (...args) => JSON.parse(execFileSync(executable, [...cliArgs, ...args], cliOptions));
const sqlite = (script, ...args) => execFileSync(python, ['-c', script, join(home, 'flock.sqlite3'), ...args], cliOptions);
let daemon;

async function start() {
  const child = spawn(executable, [...cliArgs, 'run', '--fixed', '--port', '0'], {
    env, stdio: ['ignore', 'pipe', 'pipe'],
  });
  const closed = once(child, 'close');
  daemon = { child, closed };
  let stdout = '';
  let stderr = '';
  child.stderr.setEncoding('utf8').on('data', (text) => { stderr += text; });
  const ready = new Promise((resolve, reject) => {
    child.stdout.setEncoding('utf8').on('data', (text) => {
      stdout += text;
      const match = stdout.match(/Flock Web UI at (http:\/\/127\.0\.0\.1:\d+)/);
      if (match) resolve(match[1]);
    });
    child.once('error', reject);
    child.once('close', (code) => reject(new Error(`Flock exited during startup (${code}): ${stderr}`)));
  });
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`Flock did not start: ${stderr}`)), 5000);
  });
  try {
    daemon.origin = await Promise.race([ready, timeout]);
  } finally {
    clearTimeout(timer);
  }
}

async function stop() {
  if (!daemon) return;
  daemon.child.kill();
  await daemon.closed;
  daemon = undefined;
}

async function api(path, { actor = root, body, status = 200, origin = daemon.origin } = {}) {
  const headers = actor ? { Authorization: actor } : {};
  if (body) Object.assign(headers, { Origin: origin, 'Content-Type': 'application/json', 'X-Flock-Action': '1' });
  const response = await fetch(`${daemon.origin}/api/${path}`, {
    headers, method: body ? 'POST' : 'GET', body: JSON.stringify(body), signal: AbortSignal.timeout(5000),
  });
  const text = await response.text();
  assert.equal(response.status, status, `${path}: ${text}`);
  return JSON.parse(text);
}

async function waitForTask(messageId, state) {
  const deadline = Date.now() + 5000;
  let task;
  do {
    task = cli('tasks', 'release').find((item) => item.sourceMessageId === messageId);
    if (task?.state === state) return task;
    await delay(50);
  } while (Date.now() < deadline);
  assert.fail(`Expected the task for message ${messageId} to reach ${state}, got ${task?.state}`);
}

function configuredSchedule() {
  return JSON.parse(sqlite(`
import json, sqlite3, sys
from pathlib import Path
with sqlite3.connect(Path(sys.argv[1]).as_uri() + '?mode=ro', uri=True) as db:
    result = db.execute("SELECT status, next_run_at FROM automations WHERE bot_id='release' AND config_id='yearly'").fetchone()
print(json.dumps(result))
`));
}

function prepareStoreWithoutDeletedBots() {
  cli('rules', 'list');
  sqlite(`
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    db.execute('DROP TABLE deleted_bots')
`);
}

const deleteBot = (botId, options) => api('bots/delete', { body: { botId }, ...options });
const botIds = (bots) => bots.map((bot) => bot.id).sort();

try {
  writeFileSync(identities, JSON.stringify([
    { person: 'administrator', authorization: root, adminRoot: true },
    { person: 'owner', authorization: owner },
    { person: 'member', authorization: member },
  ]), { mode: 0o600 });
  const botDirectory = join(home, 'bots', 'release');
  const configPath = join(botDirectory, 'config.json');
  mkdirSync(botDirectory, { recursive: true });
  const futureScheduleMonth = (new Date().getUTCMonth() + 6) % 12 + 1;
  const releaseConfig = JSON.stringify({
    name: 'Release', modelProvider: 'chatgpt', model: 'demo', environmentId: 'demo-env', projectId: 'demo-project',
    cron: [{ id: 'yearly', schedule: `0 0 1 ${futureScheduleMonth} *`, timezone: 'Etc/UTC', request: 'Check the next release' }],
  });
  writeFileSync(configPath, releaseConfig);
  prepareStoreWithoutDeletedBots();
  await start();
  const [scheduleStatus, nextRun] = configuredSchedule();
  assert.equal(scheduleStatus, 'active');
  assert.ok(nextRun > Math.floor(Date.now() / 1000));

  const profile = { name: 'Scout', avatar: { kind: 'shape', example: 'sprout', color: '#69c9ac' } };
  const scout = await api('bots', { actor: owner, body: profile, status: 201 });
  await api('bot-settings/members', { actor: owner, body: { botId: scout.id, person: 'member', add: true } });
  await api('settings/installation/channels', {
    body: { action: 'saveBridge', type: 'slack', id: 'fixture', port: 8080, credential: 'fixture-only-bridge-credential-000' },
  });
  const link = { action: 'setSender', type: 'slack', id: 'fixture', sender: { id: 'U123', personId: 'owner' } };
  await api('bot-settings/channels', { body: { botId: scout.id, ...link } });
  assert.equal((await api('bots', { actor: owner })).find((bot) => bot.id === scout.id).canDelete, true);
  assert.equal((await api(`view?bot=${scout.id}`, { actor: owner })).bot.canDelete, true);
  assert.equal((await api('bots', { actor: member })).find((bot) => bot.id === scout.id).canDelete, false);
  assert.equal((await api(`view?bot=${scout.id}`, { actor: member })).bot.canDelete, false);
  assert.deepEqual(await deleteBot(scout.id, { actor: null, status: 401 }), { error: 'identity_required' });
  assert.deepEqual(await deleteBot(scout.id, { actor: member, status: 403 }), { error: 'access_denied' });
  assert.deepEqual(await deleteBot(scout.id, { actor: owner, origin: 'https://example.invalid', status: 403 }), { error: 'invalid_origin' });
  assert.deepEqual(await deleteBot('missing', { status: 404 }), { error: 'bot_not_found' });
  assert.deepEqual(await deleteBot(scout.id, { actor: owner }), { ok: true });
  assert.deepEqual(botIds(await api('bots')), ['release']);

  const replacement = await api('bots', { actor: owner, body: profile, status: 201 });
  assert.equal(replacement.name, scout.name);
  assert.notEqual(replacement.id, scout.id);
  assert.deepEqual(await api('bots', { actor: member }), [], 'The replacement inherited the deleted Bot’s members');
  const { bridges } = await api('bot-settings/channels', { body: { botId: replacement.id, ...link } });
  assert.deepEqual(bridges[0].senders, [{ id: 'U123', personId: 'owner' }]);

  const botId = 'release';
  const { messageId } = await api('messages', {
    body: { botId, clientMessageId: 'unfinished', text: 'ask: Which release should I check?' }, status: 201,
  });
  const waiting = await waitForTask(messageId, 'waiting');
  assert.deepEqual(await deleteBot(botId, { status: 409 }), { error: 'bot_busy' });
  assert.deepEqual(botIds(await api('bots')), [botId, replacement.id].sort());
  assert.deepEqual(configuredSchedule(), ['active', nextRun]);
  cli('cancel', String(waiting.id));
  await waitForTask(messageId, 'cancelled');
  const history = cli('history', String(waiting.id));
  assert.ok(history.some(({ kind }) => kind === 'cancelled'));

  assert.deepEqual(await deleteBot(botId), { ok: true });
  assert.deepEqual(botIds(await api('bots')), [replacement.id]);
  assert.deepEqual(botIds(cli('bots')), [replacement.id]);
  assert.deepEqual(configuredSchedule(), ['inactive', null]);
  assert.equal(readFileSync(configPath, 'utf8'), releaseConfig);
  const rejected = spawnSync(executable, [...cliArgs, 'message', botId, 'This should not be queued'], cliOptions);
  assert.equal(rejected.status, 1);

  await stop();
  await start();
  assert.deepEqual(botIds(await api('bots')), [replacement.id]);
  assert.deepEqual(botIds(cli('bots')), [replacement.id]);
  assert.deepEqual(configuredSchedule(), ['inactive', null]);
  assert.deepEqual(cli('history', String(waiting.id)), history);
  assert.deepEqual(cli('tasks', botId).map(({ id, state }) => ({ id, state })), [{ id: waiting.id, state: 'cancelled' }]);
} finally {
  await stop();
  rmSync(home, { recursive: true, force: true });
}
