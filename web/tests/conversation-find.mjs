import { execFile, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { promisify } from 'node:util';

async function exercise(mountConversationFind) {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  const wait = () => new Promise(resolve => setTimeout(resolve, 200));
  const key = (target, key, extra = {}) => target.dispatchEvent(new KeyboardEvent('keydown', {
    bubbles: true, cancelable: true, key, ...extra,
  }));
  const toggle = $('#find-conversation');
  const panel = $('#conversation-find');
  const draft = $('#message');
  const input = panel.querySelector('input');
  const status = $('.find-status');
  const more = $('.find-more');
  const release = { id: 'release' };
  const triage = { id: 'triage' };
  const calls = [];
  const opened = [];
  const item = (id, text, kind = 'conversation_message') => ({ id, time: 1700000000 + id, kind, data: { text } });
  let holdRequest = false;
  let failRequest = false;
  let pending;
  const find = mountConversationFind({
    toggle, panel,
    readPage(scope, before, signal) {
      calls.push({ scope, before, signal });
      if (failRequest) return Promise.reject(Object.assign(new Error('access_denied'), { status: 403 }));
      if (holdRequest) return new Promise(resolve => { pending = resolve; });
      if (scope === triage) return Promise.resolve({ events: [item(3, 'Triage update')], olderBefore: 0 });
      if (!before) return Promise.resolve({ events: [item(40, 'Handoff [draft] today'),
        item(41, 'Use the [redacted] marker in exported reports.'), item(42, 'Handoff [draft]', 'bot_message'),
        ...Array.from({ length: 7 }, (_, index) => item(43 + index, `Short window match ${index + 1}`)),
      ], olderBefore: 40 });
      if (before === 40) return Promise.resolve({ events: [item(11, 'Old handoff [draft] <img src=x>')], olderBefore: 11 });
      const run = { environmentId: 'local', threadId: 'release', runId: 'run' };
      if (before === 11) return Promise.resolve({ events: [{
        ...item(10, 'Verified release report', 'completed'), taskId: 1,
        data: { text: 'Verified release report', revisionId: 1, run },
      }], olderBefore: 10 });
      if (before === 10) return Promise.resolve({ events: [{
        ...item(9, '', 'report'), taskId: 1,
        data: { report: 'Verified release report', status: 'completed', truncated: false, revisionId: 1, run, resultRunId: 'run' },
      }], olderBefore: 9 });
      return Promise.resolve({ events: [item(7, 'Earliest handoff [draft]')], olderBefore: 0 });
    },
    describeAuthor: () => 'You',
    openMatch: (scope, id, signal) => opened.push([scope, id, signal]),
  });
  function type(value) {
    input.value = value;
    input.dispatchEvent(new Event('input'));
  }

  check(toggle.hidden && panel.hidden, 'Find must start unavailable without a selected Bot');
  find.setContext(release);
  $('#composer-container').hidden = false;
  draft.value = 'Unsent draft';
  draft.focus();
  check(!key(draft, 'f', { ctrlKey: true }) && document.activeElement === input &&
    toggle.getAttribute('aria-expanded') === 'true', 'Find shortcut did not focus the search field');
  type('HANDOFF [DRAFT]');
  await wait();
  check(calls.length === 4 && calls[0].before === 0 && calls[1].before === 40 &&
    calls.every(call => call.scope === release), 'Find did not read a bounded batch for the selected Bot');
  check(status.textContent === '1 of 2 matches so far' && !more.hidden &&
    $('.find-results').textContent.includes('Old handoff') && !$('.find-results img'),
    'Find lost an older match, exposed DM text, or interpreted message markup');
  key(input, 'ArrowDown');
  check(status.textContent === '2 of 2 matches so far', 'ArrowDown did not select the older match');
  key(input, 'Enter', { isComposing: true });
  check(!opened.length, 'Enter while composing opened a result');
  key(input, 'Enter');
  check(opened.length === 1 && opened[0][0] === release && opened[0][1] === 11,
    'Enter did not open the selected Bot and exact matching message');
  more.click();
  await wait();
  check(status.textContent === '2 of 3 matches' && calls.at(-1).before === 9 && more.hidden,
    'Earlier search did not continue from its own cursor or settle the final count');
  $('.find-next').click();
  check(opened.at(-1)[1] === 7, 'Next did not immediately jump to the earliest result');
  $('.find-previous').click();
  check(opened.at(-1)[1] === 11, 'Previous did not immediately jump to the older result');

  const literalQuery = 'the [redacted] marker';
  type(literalQuery);
  check(opened.at(-1)[2].aborted, 'Changing the query did not cancel the previous result jump');
  await wait();
  more.click();
  await wait();
  check(status.textContent === '1 of 1 match' && $('.find-match mark')?.textContent === literalQuery,
    'Find did not match the literal [redacted] text in the displayed message');
  key(input, 'Escape', { isComposing: true });
  check(input.value === literalQuery && !panel.hidden, 'Escape cleared Find during composition');
  check(!key(input, 'Escape') && !panel.hidden && !input.value && !status.textContent &&
    document.activeElement === input, 'Escape did not clear the query and keep Find focused');
  key(input, 'Escape');
  check(panel.hidden && document.activeElement === draft && draft.value === 'Unsent draft',
    'Escape did not restore the composer focus and draft');

  toggle.click();
  type('Verified release report');
  await wait();
  check(status.textContent === '1 of 1 match so far', 'Find duplicated a report split across history pages');
  key(input, 'Enter');
  check(opened.at(-1)[1] === 10, 'Find did not open the deduplicated completion');

  type('absent from the conversation');
  await wait();
  more.click();
  await wait();
  check(status.textContent === 'No matches' && !$('.find-match'), 'Find reported a match for an absent query');

  holdRequest = true;
  type('handoff');
  await wait();
  const stale = calls.at(-1);
  find.setContext(triage);
  check(stale.signal.aborted && panel.hidden && !input.value, 'Bot switch left a request or old query active');
  holdRequest = false;
  toggle.click();
  type('update');
  pending({ events: [item(40, 'Stale handoff')], olderBefore: 0 });
  await wait();
  check(status.textContent === '1 of 1 match' && $('.find-match').textContent.includes('Triage update') &&
    calls.at(-1).scope === triage, 'A late response entered the new Bot’s results');
  failRequest = true;
  type('update again');
  await wait();
  check(status.textContent === 'Conversation unavailable' && !$('.find-match') && more.hidden,
    'A denied history request exposed results or offered another page');

  check(innerWidth === 390 && innerHeight === 550, 'Expected a 390 × 550 browser viewport');
  failRequest = false;
  find.setContext(release);
  const fullDraft = Array.from({ length: 12 }, (_, index) => `Draft line ${index + 1}`).join('\n');
  draft.value = fullDraft;
  draft.style.height = '300px';
  const height = draft.getBoundingClientRect().height;
  check(height === 220, 'The draft must fill the textarea before Find opens');
  $('#jump-to-latest').hidden = false;
  draft.focus();
  key(draft, 'f', { ctrlKey: true });
  type('Short window match');
  await wait();
  const results = $('.find-results');
  for (let index = 0; index < 6; index++) key(input, 'ArrowDown');
  const selected = $('.find-match[aria-current="true"]').getBoundingClientRect();
  const list = results.getBoundingClientRect();
  const panelBottom = panel.getBoundingClientRect().bottom;
  check(results.children.length === 7 && results.scrollTop > 0 &&
    selected.top >= list.top - 1 && selected.bottom <= list.bottom + 1,
    'Keyboard navigation must render all matches and scroll the selected result into view');
  const available = $('#jump-to-latest').getBoundingClientRect().top - panelBottom;
  check(!more.hidden && more.getBoundingClientRect().bottom < panelBottom,
    'Search earlier must stay visible inside Find');
  check(available >= 64, `Find left only ${available}px above Jump to latest`);
  check($('#send').getBoundingClientRect().bottom <= innerHeight &&
    draft.getBoundingClientRect().bottom <= innerHeight && draft.scrollHeight > draft.clientHeight,
    'The compact draft must remain visible and scrollable with Send in the viewport');
  key(input, 'Escape');
  key(input, 'Escape');
  check(draft.value === fullDraft && draft.getBoundingClientRect().height === height &&
    document.activeElement === draft, 'Closing Find must restore the full draft field');
}

const assets = new URL('../assets/', import.meta.url);
const read = (name) => readFileSync(new URL(name, assets), 'utf8');
const directory = mkdtempSync(join(tmpdir(), 'flock-conversation-find-'));
try {
  const html = read('index.html').replace(/<link\b[^>]*>/g, '').replace('<script type="module" src="/app.js"></script>', `
    <style>${['style.css', 'sidebar.css', 'conversation.css', 'conversation-find.css', 'composer.css'].map(read).join('\n')}</style>
    <script type="module">
      import { mountConversationFind } from ${JSON.stringify(new URL('conversation-find.mjs', assets).href)};
      try {
        await (${exercise})(mountConversationFind);
        parent.document.documentElement.dataset.flockResult = 'pass';
      } catch (error) {
        parent.document.documentElement.dataset.flockResult = error.message;
      }
    </script>`);
  writeFileSync(join(directory, 'index.html'), html);
  const fixture = join(directory, 'test.html');
  writeFileSync(fixture, '<!doctype html><iframe src="index.html" width="390" height="550" style="border:0"></iframe>');
  const output = execFileSync(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--allow-file-access-from-files', `--user-data-dir=${directory}`,
    '--virtual-time-budget=4000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
  const result = output.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the find checks');
} finally {
  rmSync(directory, { recursive: true, force: true });
}

function appFixtures() {
  window.EventSource = undefined;
  const bot = { id: 'release', name: 'Release', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  const time = Math.floor(Date.now() / 1000) - 600;
  const longText = Array.from({ length: 24 }, (_, i) => `Long line ${i + 1} for folding needlecheck`).join('\n');
  const events = [
    { id: 1, kind: 'conversation_message', time, data: { text: longText } },
    { id: 2, kind: 'conversation_reply', time: time + 10, data: { text: 'Second match needlecheck' } },
  ];
  window.fetch = async (path) => {
    if (path === '/api/bots') return Response.json([bot]);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: true });
    if (path.startsWith('/api/view?')) {
      return Response.json({
        bot, events, olderBefore: 0, presence: 'idle', activeSessions: [], activeTasks: [], parallelTasks: [],
        sessionTasks: [], sessionsBefore: 0, sessionCursor: 0, waits: [], canDecide: true,
        composerPerson: 'local-owner', limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
      });
    }
    if (path.startsWith('/api/conversation?')) return Response.json({ events, olderBefore: 0 });
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exerciseAppFolding() {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 60; attempt++) {
      if (predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 15));
    }
    check(predicate(), message);
  }
  await waitFor(() => $('#event-1 .bubble[data-fold="folded"]') && $('#find-conversation'),
    'Long message did not initially fold');
  $('#find-conversation').click();
  await waitFor(() => !$('#conversation-find').hidden, 'Find panel did not open');
  const search = $('#conversation-find input');
  search.value = 'needlecheck';
  search.dispatchEvent(new Event('input', { bubbles: true }));
  await waitFor(() => document.querySelectorAll('.find-match').length === 2, 'Find did not return both matches');
  // First match is #event-2 (newest); clicking next jumps to #event-1 and unfolds it.
  $('.find-next').click();
  await waitFor(() => $('#event-1')?.classList.contains('conversation-source-target'),
    'Next match button did not jump to the folded message');
  check($('#event-1 .bubble')?.dataset.fold === 'unfolded', 'Jumping to a folded match did not unfold it');
}

const appIndex = read('index.html').replace('<script type="module" src="/app.js"></script>', `
  <script>(${appFixtures})();</script>
  <script type="module">
    import '/app.js';
    try {
      await (${exerciseAppFolding})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error?.stack || error?.message || String(error);
    }
  </script>`);
const contentTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
const server = createServer((request, response) => {
  const { pathname } = new URL(request.url, 'http://localhost');
  try {
    response.setHeader('Content-Type', pathname === '/' ? 'text/html' : contentTypes[extname(pathname)] ?? 'application/octet-stream');
    response.end(pathname === '/' ? appIndex : readFileSync(new URL(`.${pathname}`, assets)));
  } catch {
    response.writeHead(404).end();
  }
});
const appDirectory = mkdtempSync(join(tmpdir(), 'flock-conversation-find-app-'));
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', `--user-data-dir=${appDirectory}`, '--virtual-time-budget=4000', '--dump-dom',
    `http://127.0.0.1:${server.address().port}/?bot=release`], { encoding: 'utf8', timeout: 25000 });
  const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish folding-on-jump checks');
} finally {
  server.close();
  rmSync(appDirectory, { recursive: true, force: true });
}
