import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { promisify } from 'node:util';

function fixtures() {
  window.EventSource = undefined;
  window.notificationRequests = 0;
  const fetchFromServer = window.fetch.bind(window);
  const bots = [{ id: 'release', name: 'Release' }, { id: 'triage', name: 'Triage' }];
  const events = [];
  const scopes = { release: 0, triage: 0 };
  window.notices = [];
  window.Notification = class {
    static permission = 'granted';
    static requestPermission() { return 'granted'; }
    constructor() { notices.push(this); }
    close() {}
  };
  localStorage.setItem('flock-notifications', 'on');
  window.addAlert = bot => {
    events.push({ kind: 'reply', botId: bot, botName: bots.find(entry => entry.id === bot).name, id: events.length + 1 });
    window.dispatchEvent(new Event('flock-notifications'));
  };
  window.fetch = async (path, request) => {
    const url = new URL(path, location.origin);
    if (path === '/api/bots' && request?.method === 'POST') {
      const { name } = JSON.parse(request.body);
      await new Promise(resolve => { window.finishBotSave = resolve; });
      const bot = { id: 'new-bot', name, model: 'demo' };
      bots.push(bot);
      return Response.json(bot);
    }
    if (path === '/api/bots') return Response.json(bots);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (url.pathname === '/api/conversation') {
      const bot = bots.find(entry => entry.id === url.searchParams.get('bot'));
      return Response.json({ bot, events: [], olderBefore: 0, presence: 'idle', waits: [],
        activeSessions: [], sessionTasks: [], sessionsBefore: 0, canDecide: true });
    }
    if (path === '/api/notifications') {
      window.notificationRequests++;
      const { after } = JSON.parse(request.body);
      return Response.json({ through: events.length, scope: scopes,
        events: after == null ? [] : events.filter(event => event.id > after), inbox: [] });
    }
    return fetchFromServer(path, request);
  };
}

async function exercise() {
  const byId = id => document.getElementById(id);
  const editor = byId('bot-editor');
  const selected = () => new URLSearchParams(location.search).get('bot');
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  async function waitFor(condition, message) {
    for (let attempt = 0; attempt < 50 && !condition(); ++attempt)
      await new Promise(resolve => setTimeout(resolve, 10));
    check(condition(), message);
  }
  function draft(name) {
    byId('new-bot-button').click();
    editor.querySelector('button[data-preset="custom"]').click();
    byId('studio-bot-name').value = name;
    byId('studio-bot-name').dispatchEvent(new Event('input'));
  }

  await waitFor(() => window.notificationRequests > 0 && byId('profile-name').textContent === 'Release', 'The initial Bot did not load');
  draft('Keep this draft');
  addAlert('triage');
  await waitFor(() => notices.length === 1, 'The first alert did not arrive');
  notices[0].onclick({ preventDefault() {} });
  check(editor.open && selected() === 'release' && byId('studio-bot-name').value === 'Keep this draft',
    'The alert changed the conversation behind the editor or discarded the draft');
  editor.querySelector('#studio-editor [data-close]').click();
  await waitFor(() => !editor.open && selected() === 'triage', 'Closing the editor lost the alert destination');

  draft('Save this draft');
  byId('studio-save').click();
  await waitFor(() => !!window.finishBotSave, 'Saving did not start');
  addAlert('release');
  await waitFor(() => notices.length === 2, 'The alert did not arrive during save');
  notices[1].onclick({ preventDefault() {} });
  check(editor.open && selected() === 'triage', 'The alert interrupted the save');
  window.finishBotSave();
  await waitFor(() => !editor.open && selected() === 'release', 'Finishing the save lost the alert destination');
  check(document.body.textContent.includes('Save this draft'), 'The saved Bot did not reach the roster');
}

const assets = new URL('../assets/', import.meta.url);
const read = path => readFile(new URL(`.${path}`, assets));
const html = (await read('/index.html')).toString().replace('<script type="module" src="/app.js"></script>', `
  <img hidden src="/test-pending.svg">
  <script>(${fixtures})();</script>
  <script type="module">
    import '/app.js';
    try { await (${exercise})(); document.documentElement.dataset.flockResult = 'pass'; }
    catch (error) { document.documentElement.dataset.flockResult = error.message; }
    finally { await fetch('/test-complete'); }
  </script>`);
const contentTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
let pendingAsset;
let testComplete = false;
const server = createServer(async (request, response) => {
  const { pathname } = new URL(request.url, 'http://localhost');
  // Keep the browser alive until the dialog's native close event has run.
  if (pathname === '/test-pending.svg' && !testComplete) { pendingAsset = response; return; }
  if (pathname === '/test-complete') {
    testComplete = true;
    pendingAsset?.writeHead(204).end();
    response.writeHead(204).end();
    return;
  }
  try {
    response.setHeader('Content-Type', pathname === '/' ? 'text/html' : contentTypes[extname(pathname)] ?? 'application/octet-stream');
    response.end(pathname === '/' ? html : await read(pathname));
  } catch {
    response.writeHead(404).end();
  }
});
const directory = await mkdtemp(join(tmpdir(), 'flock-notification-navigation-'));
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--force-prefers-reduced-motion', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', `--user-data-dir=${directory}`, '--dump-dom',
    `http://127.0.0.1:${server.address().port}/?bot=release`], { encoding: 'utf8', timeout: 25000 });
  const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the alert navigation checks');
} finally {
  server.close();
  await rm(directory, { recursive: true, force: true });
}
