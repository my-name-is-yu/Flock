import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';

function fixtures() {
  const realFetch = window.fetch.bind(window);
  window.EventSource = class extends EventTarget {
    close() {}
  };
  const bot = { id: 'release', name: 'Release', model: 'demo', paused: false, canDelete: true, presence: 'waiting' };
  const t3Wait = {
    id: 21,
    request: 'Finish deployment in T3',
    state: 'waiting',
    generation: 2,
    currentRevision: 1,
    createdAt: Math.floor(Date.now() / 1000) - 120,
    wait: {
      id: 201,
      reason: 't3_input',
      text: 'Approve tool execution in T3',
      run: { environmentId: 'local', threadId: 't3-21', link: 'http://localhost:3773/local/t3-21' },
    },
  };
  const problemWait = {
    id: 22,
    request: 'Inspect release failure',
    state: 'waiting',
    generation: 3,
    currentRevision: 2,
    createdAt: Math.floor(Date.now() / 1000) - 90,
    wait: { id: 202, reason: 'model_failure', text: 'Model call failed twice.' },
  };
  const problemTask = {
    id: 22,
    request: 'Inspect release failure',
    state: 'waiting',
    generation: 3,
    currentRevision: 2,
    createdAt: Math.floor(Date.now() / 1000) - 90,
    problem: { id: 202, reason: 'model_failure', text: 'Model call failed twice.' },
  };
  const runningTask = {
    id: 23,
    request: 'Draft changelog summary',
    state: 'running',
    generation: 4,
    currentRevision: 3,
    createdAt: Math.floor(Date.now() / 1000) - 60,
    session: { state: 'working', name: 'T3 Session' },
  };
  const questionItem = {
    botId: 'release',
    botName: 'Release',
    itemKind: 'user_question',
    itemId: 203,
    waitId: 203,
    taskId: 24,
    targetRevision: 1,
    targetGeneration: 5,
    requestTitle: 'Choose release tag',
    request: 'Choose release tag',
    prompt: 'Which release channel?',
    choices: ['stable', 'beta'],
    canDecide: true,
  };
  const questionWait = {
    id: 24,
    request: 'Choose release tag',
    state: 'waiting',
    generation: 5,
    currentRevision: 1,
    wait: { id: 203, reason: 'user_question', text: 'Which release channel?', choices: ['stable', 'beta'] },
  };
  window.__operationsCalls = [];
  window.__inboxDecideCalls = [];
  window.fetch = async (path, options = {}) => {
    if (typeof path === 'string' && path.startsWith('/result?')) return realFetch(path, options);
    const headers = { 'X-Flock-Viewer': '"local-owner"' };
    if (path === '/api/bots') return Response.json([bot], { headers });
    if (path === '/api/inbox') return Response.json({ items: [questionItem] }, { headers });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: true }, { headers });
    if (path.startsWith('/api/view?')) {
      return Response.json({
        bot,
        events: [],
        olderBefore: 0,
        presence: 'waiting',
        activeTasks: [problemTask, runningTask],
        parallelTasks: [],
        sessionTasks: [],
        sessionsBefore: 0,
        sessionCursor: 0,
        waits: [t3Wait, problemWait, questionWait],
        canDecide: true,
        composerPerson: 'local-owner',
        limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
      }, { headers });
    }
    if (path === '/api/operations') {
      window.__operationsCalls.push(JSON.parse(options.body));
      return Response.json({ accepted: true });
    }
    if (path === '/api/inbox/decide') {
      window.__inboxDecideCalls.push(JSON.parse(options.body));
      return Response.json({ accepted: true });
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exercise() {
  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => [...document.querySelectorAll(selector)];
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 60; attempt++) {
      if (predicate()) return;
      await new Promise((resolve) => setTimeout(resolve, 15));
    }
    check(predicate(), message);
  }

  await waitFor(() => $$('#wait-list .wait-row').length === 2 && $$('#session-list .request-stop').length === 2,
    'Sidebar waits and active request stop buttons did not render');

  // 1. t3_input wait appears in Needs you (#wait-list), problem wait does not duplicate there.
  check($('#wait-count')?.textContent === '2', 'Sidebar wait count did not include non-inbox t3_input wait');
  const waitTitles = $$('#wait-list .wait-row .activity-title').map((el) => el.textContent);
  check(waitTitles.includes('Finish deployment in T3') && !waitTitles.includes('Inspect release failure'),
    `Unexpected sidebar wait rows: ${JSON.stringify(waitTitles)}`);
  const t3Row = $$('#wait-list .wait-row').find((row) => row.textContent.includes('Finish deployment in T3'));
  t3Row.click();
  await waitFor(() => $('#detail')?.open && $('#detail-title')?.textContent === 'Action in T3',
    'Clicking t3_input wait in sidebar did not open detail modal');
  $('#close-detail').click();
  check(!$('#detail').open, 'Detail did not close');

  // 2. task.problem row in Requests -> Waiting opens its wait detail modal.
  const problemRow = $('#session-list [data-session-group="waiting"] button.session-row');
  check(problemRow && problemRow.textContent.includes('Inspect release failure'),
    'Problem task in Requests -> Waiting was not clickable');
  problemRow.click();
  await waitFor(() => $('#detail')?.open && $('#detail-title')?.textContent.includes('model needs attention'),
    'Clicking problem task did not open wait detail');
  $('#close-detail').click();

  // 3. Running task stop button in sidebar opens confirmation and calls POST /api/operations with waitId: 0.
  const runningStop = $('#session-list [data-session-group="working"] .request-stop');
  check(runningStop, 'Running task did not render .request-stop button');
  runningStop.click();
  await waitFor(() => $('#detail')?.open && $('#detail-title')?.textContent === 'Stop this request?',
    'Sidebar stop button did not open stop confirmation dialog');
  check(document.activeElement?.classList.contains('wait-stop-keep'),
    'Keep working button was not focused by default');
  $('#detail .button.danger').click();
  await waitFor(() => window.__operationsCalls.length === 1 && !$('#detail').open,
    'Confirming sidebar stop did not call /api/operations');
  const op1 = window.__operationsCalls[0];
  check(op1.operation === 'cancel' && op1.taskId === 23 && op1.waitId === 0,
    `Sidebar stop sent wrong payload: ${JSON.stringify(op1)}`);

  // 4. Task-wait inbox card (user_question) shows Stop this request inline confirmation and routes cancel through /api/operations.
  $('#needs-you').click();
  await waitFor(() => !$('#decision-layer').hidden && $('#decision-item .board-inbox-stop .wait-cancel'),
    'Inbox question card did not show Stop this request button');
  $('#decision-item .board-inbox-stop .wait-cancel').click();
  await waitFor(() => $('#decision-item .board-inbox-stop .wait-stop-confirm'),
    'Inbox stop button did not open inline confirmation');
  check(document.activeElement?.classList.contains('wait-stop-keep'),
    'Inbox stop confirmation did not focus Keep working');
  $('#decision-item .board-inbox-stop .button.danger').click();
  await waitFor(() => window.__operationsCalls.length === 2,
    'Inbox stop did not route cancel to /api/operations');
  check(window.__inboxDecideCalls.length === 0, 'Inbox stop wrongly called /api/inbox/decide');
  const op2 = window.__operationsCalls[1];
  check(op2.operation === 'cancel' && op2.taskId === 24 && op2.waitId === 203,
    `Inbox stop sent wrong payload: ${JSON.stringify(op2)}`);
}

const assets = new URL('../assets/', import.meta.url);
const read = (path) => readFile(new URL(`.${path}`, assets));
const index = (await read('/index.html')).toString().replace('<script type="module" src="/app.js"></script>', () => `
  <script>(${fixtures})();</script>
  <script type="module">
    try {
      await import('/app.js');
      await (${exercise})();
      await fetch('/result?passed');
    } catch (error) {
      await fetch('/result?error=' + encodeURIComponent(error?.stack || error?.message || String(error)));
    }
  </script>`);
const contentTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
const server = createServer(async (request, response) => {
  const { pathname, searchParams } = new URL(request.url, 'http://localhost');
  if (pathname === '/result') {
    response.writeHead(204).end();
    server.emit('browserResult', searchParams.get('error'));
    return;
  }
  try {
    response.setHeader('Content-Type', pathname === '/' ? 'text/html; charset=utf-8' : contentTypes[extname(pathname)] ?? 'application/octet-stream');
    response.end(pathname === '/' ? index : await read(pathname));
  } catch {
    response.writeHead(404).end();
  }
});

const directory = await mkdtemp(join(tmpdir(), 'flock-stop-requests-dom-'));
let chrome;
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  chrome = spawn(process.argv[2], [
    '--headless', '--disable-gpu', '--no-first-run', '--disable-background-networking',
    `--user-data-dir=${directory}`, `http://127.0.0.1:${server.address().port}/?bot=release`,
  ], { stdio: 'ignore' });
  const result = once(server, 'browserResult', { signal: AbortSignal.timeout(20_000) });
  const exited = once(chrome, 'exit').then(([code]) => { throw new Error(`Chrome exited before the report (${code})`); });
  const [failure] = await Promise.race([result, exited]);
  if (failure) throw new Error(failure);
} finally {
  if (chrome?.pid && chrome.exitCode === null && !chrome.signalCode) {
    const closed = once(chrome, 'exit');
    chrome.kill();
    await closed;
  }
  server.closeAllConnections();
  server.close();
  await rm(directory, { recursive: true, force: true });
}
