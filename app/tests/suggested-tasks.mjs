import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

const [, , executable, chrome] = process.argv;
const directory = mkdtempSync(join(tmpdir(), 'flock-suggested-tasks-'));
const home = join(directory, 'home');
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith('FLOCK_')));
const cli = (...args) => execFileSync(executable, ['--home', home, ...args], { env, encoding: 'utf8', timeout: 5000 });
const tasks = () => JSON.parse(cli('tasks', 'release'));
const processes = [];

function launch(command, args, stdio) {
  const child = spawn(command, args, { env, stdio });
  processes.push({ child, closed: once(child, 'close') });
  return child;
}

function devTools(browser) {
  const pending = new Map();
  const messages = [];
  let nextId = 0;
  let buffer = '';
  browser.stdio[4].setEncoding('utf8').on('data', chunk => {
    buffer += chunk;
    const frames = buffer.split('\0');
    buffer = frames.pop();
    for (const frame of frames) {
      const message = JSON.parse(frame);
      const request = pending.get(message.id);
      if (!request) {
        if (message.method === 'Network.eventSourceMessageReceived') messages.push(message.params);
        continue;
      }
      pending.delete(message.id);
      if (message.error) request.reject(new Error(message.error.message));
      else request.resolve(message.result);
    }
  });
  browser.once('close', code => {
    for (const { reject } of pending.values()) reject(new Error(`Chrome exited (${code})`));
  });
  const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    const id = ++nextId;
    pending.set(id, { resolve, reject });
    browser.stdio[3].write(JSON.stringify({ id, method, params, sessionId }) + '\0');
  });
  return { send, messages };
}

async function waitFor(check, failure) {
  const deadline = Date.now() + 8000;
  while (Date.now() < deadline) {
    if (await check()) return;
    await delay(40);
  }
  assert.fail(failure);
}

try {
  const bot = join(home, 'bots', 'release');
  mkdirSync(bot, { recursive: true });
  writeFileSync(join(bot, 'config.json'), JSON.stringify({
    name: 'Release', modelProvider: 'chatgpt', model: 'demo', environmentId: 'demo-env', projectId: 'demo-project',
  }));
  const daemon = launch(executable, ['--home', home, 'run', '--fixed', '--port', '0'], ['ignore', 'pipe', 'inherit']);
  let stdout = '';
  daemon.stdout.setEncoding('utf8').on('data', text => { stdout += text; });
  const origin = () => stdout.match(/Flock Web UI at (http:\/\/127\.0\.0\.1:\d+)/)?.[1];
  await waitFor(origin, 'Flock did not start');

  const browser = launch(chrome, ['--headless', '--disable-gpu', '--no-first-run', '--disable-background-networking',
    '--remote-debugging-pipe', `--user-data-dir=${join(directory, 'chrome')}`],
  ['ignore', 'ignore', 'ignore', 'pipe', 'pipe']);
  const { send, messages } = devTools(browser);
  const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
  const page = (method, params) => send(method, params, sessionId);
  async function evaluate(expression) {
    const { result, exceptionDetails } = await page('Runtime.evaluate', { expression, returnByValue: true });
    if (exceptionDetails) throw new Error(exceptionDetails.exception?.description ?? exceptionDetails.text);
    return result.value;
  }
  const card = '.suggested-tasks-card';
  const rows = `document.querySelectorAll('${card} .task-suggestion')`;
  const click = (index, selector) => evaluate(`${rows}[${index}].querySelector(${JSON.stringify(selector)}).click()`);
  const ready = `!document.querySelector('${card}[aria-busy]')`;
  const started = count => () => evaluate(`${ready} && document.querySelectorAll('${card} a').length === ${count}`);
  const noApprovals = () => evaluate('!document.querySelector(".policy-inline-card") && document.querySelector("#wait-count").hidden');

  await page('Network.enable');
  await page('Page.navigate', { url: `${origin()}/?bot=release` });
  await waitFor(() => messages.some(event => event.eventName === 'ready'), 'The change feed did not connect');
  await waitFor(() => evaluate('!document.querySelector("#composer-container").hidden'), 'The composer did not appear');
  await evaluate(`(() => {
    const message = document.querySelector('#message');
    message.value = 'Check the release notes';
    message.dispatchEvent(new Event('input', { bubbles: true }));
    document.querySelector('#send').click();
  })()`);
  await waitFor(() => tasks().some(task => task.request === 'Check the release notes' && task.state === 'completed'),
    'The direct request did not start');
  assert.equal(await evaluate(`${rows}.length`), 0);

  cli('mode', 'supervised');
  const changes = () => messages.filter(event => event.eventName === 'change').length;
  const before = changes();
  cli('message', 'release', 'Suggest follow-up tasks');
  await waitFor(() => evaluate(`${rows}.length === 5`), 'The live conversation did not show the five suggestions');
  assert.ok(changes() > before, 'The open browser received no change-feed events');
  assert.equal(tasks().length, 1);
  assert.equal(await evaluate(`document.querySelector('${card}').previousElementSibling.matches('.event-message.bot')`), true);
  assert.equal(await evaluate(`[...${rows}].every(row =>
    [...row.querySelectorAll('.task-suggestion-details p')].every(line => line.textContent.trim()))`), true);
  assert.equal(await noApprovals(), true);

  await click(0, 'button.button');
  await waitFor(started(1), 'Start did not update the first row');
  await click(1, 'button[aria-label^="Edit:"]');
  assert.equal(await evaluate('document.activeElement.matches(".task-suggestion-request")'), true);
  const edited = 'Check the integration tests for duplicate retry coverage';
  await evaluate(`(() => {
    const field = ${rows}[1].querySelector('textarea');
    field.value = ${JSON.stringify(edited)};
    field.form.requestSubmit();
  })()`);
  await waitFor(started(2), 'The edited task did not start');
  assert.ok(tasks().some(task => task.request === edited));
  await click(2, 'button[aria-label^="Dismiss:"]');
  await waitFor(() => evaluate(`${ready} && ${rows}[2].querySelector('.task-suggestion-status')?.textContent === 'Dismissed'`),
    'Dismiss did not update the third row');
  await evaluate(`document.querySelector('${card} footer button').click()`);
  await waitFor(started(4), 'Start all did not start both remaining suggestions');
  assert.equal(tasks().length, 5);
  await waitFor(() => tasks().every(task => task.state === 'completed'), 'The suggested tasks did not complete in Supervised mode');
  assert.equal(await noApprovals(), true);

  await evaluate('window.beforeSuggestedTasksReload = true');
  await page('Page.reload');
  await waitFor(() => evaluate(`!window.beforeSuggestedTasksReload && ${rows}.length === 5`), 'The card did not survive a reload');
  assert.equal(await started(4)(), true);
  assert.equal(await evaluate(`${rows}[1].querySelector('.task-suggestion-title').textContent`), edited);
  assert.equal(await evaluate(`${rows}[2].querySelector('.task-suggestion-status').textContent`), 'Dismissed');
  assert.equal(await evaluate(`document.querySelector('${card} footer').hidden`), true);
  const href = await evaluate(`${rows}[1].querySelector('a').getAttribute('href')`);
  await click(1, 'a');
  await waitFor(() => evaluate(`document.activeElement.id === ${JSON.stringify(href.slice(1))}`),
    'The started task link did not jump to its request');

  const dismissed = await evaluate(`${rows}[2].querySelector('.task-suggestion-title').textContent`);
  cli('message', 'release', 'Suggest follow-up tasks');
  const latestRows = `[...document.querySelectorAll('${card}')].at(-1).querySelectorAll('.task-suggestion')`;
  await waitFor(() => evaluate(`document.querySelectorAll('${card}').length === 2 && ${latestRows}.length === 4`),
    'The dismissed work was suggested again');
  assert.equal(await evaluate(`[...${latestRows}].some(row =>
    row.querySelector('.task-suggestion-title').textContent === ${JSON.stringify(dismissed)})`), false);

  await evaluate(`(() => {
    const row = ${latestRows}[1];
    row.querySelector('button[aria-label^="Edit:"]').click();
    row.querySelector('textarea').value = 'Keep this draft when another row changes';
  })()`);
  const view = await (await fetch(`${origin()}/api/view?bot=release`)).json();
  const [{ id }] = view.events.findLast(event => event.data.suggestedTasks).data.suggestedTasks;
  const response = await fetch(`${origin()}/api/suggested-tasks/dismiss`, {
    method: 'POST',
    headers: { Origin: origin(), 'Content-Type': 'application/json', 'X-Flock-Action': '1' },
    body: JSON.stringify({ botId: 'release', id }),
  });
  assert.equal(response.ok, true, await response.text());
  await waitFor(() => evaluate(`${latestRows}[0].querySelector('.task-suggestion-status')?.textContent === 'Dismissed'`),
    'The open browser did not receive the dismissal');
  assert.equal(await evaluate(`document.activeElement === ${latestRows}[1].querySelector('textarea') &&
    document.activeElement.value === 'Keep this draft when another row changes'`), true);
} finally {
  for (const { child } of processes) if (child.exitCode === null) child.kill();
  await Promise.all(processes.map(({ closed }) => closed));
  rmSync(directory, { recursive: true, force: true });
}
