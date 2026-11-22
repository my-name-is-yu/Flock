import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

async function exercise(mountBoardInbox) {
  const byId = (id) => document.getElementById(id);
  const check = (value, message) => { if (!value) throw new Error(message); };
  const settled = async () => { await new Promise((resolve) => setTimeout(resolve, 0)); };
  const dialog = byId('board-inbox');
  const filters = byId('work-view-status-label');
  const cards = byId('work-view-items');
  const message = byId('work-view-message');
  const text = '<img data-user-content> branch?';
  const target = { botId: 'release', itemKind: 'user_question', itemId: 2, waitId: 2, taskId: 1,
    targetRevision: 3, targetGeneration: 4 };
  const question = { ...target, botName: 'Release', requestTitle: 'Draft notes', request: 'Draft notes for main',
    prompt: text, choices: ['main'], canDecide: true };
  const revised = { ...question, itemId: 5, waitId: 5, targetRevision: 6, targetGeneration: 7 };
  const secret = { ...question, itemKind: 'protected_input', itemId: 3, requestTitle: 'Protected input' };
  const review = { ...question, itemKind: 'pr_review', itemId: 4, canDecide: false,
    prTitle: 'Review link', prUrl: 'data:text/plain,ignored' };
  const calls = [];
  let decided = false;
  mountBoardInbox(async (path, data) => {
    calls.push([path, data]);
    if (path === 'bots') return [{ id: 'release', name: 'Release' }];
    if (path.startsWith('board')) return { tasks: [] };
    if (path === 'inbox/decide') {
      if (data.waitId !== revised.waitId) throw new Error('request_changed');
      decided = true;
      return { accepted: true };
    }
    if (path === 'inbox/history') return { decisions: [] };
    return { items: decided ? [] : calls.filter(([route]) => route === 'inbox').length === 1
      ? [question, secret, review] : [revised] };
  });

  check(!dialog.open && message.getAttribute('role') === 'status', 'Missing closed dialog or live status');
  document.querySelector('[data-work-view="board"]').click();
  await settled();
  check(dialog.open && byId(dialog.getAttribute('aria-labelledby')).textContent === 'Task board' && !filters.hidden,
    'The board lost its accessible title or status filter');
  byId('work-view-bot').value = 'release';
  const status = byId('work-view-status');
  status.value = 'blocked';
  status.dispatchEvent(new Event('change'));
  await settled();
  check(calls.at(-1)[0] === 'board?bot=release&status=blocked', 'The filter did not narrow the board request');
  byId('work-view-close').click();
  await settled();
  document.querySelector('[data-work-view="inbox"]').click();
  await settled();
  check(filters.hidden && getComputedStyle(filters).display === 'none', 'Inbox showed the board status filter');
  check(cards.textContent.includes(text) && !cards.querySelector('img,a'), 'User text or an unsafe URL entered the DOM');
  check(!cards.lastElementChild.querySelector('form') && cards.lastElementChild.textContent.includes('View only'),
    'A view-only item showed decision controls');
  const password = cards.querySelector('input[type=password]');
  check(password?.labels[0].textContent === 'Protected value', 'The protected value is not a labeled password field');
  const form = cards.querySelector('form');
  form.querySelector('button[type=button]').click();
  const answer = form.querySelector('textarea');
  check(answer.value === 'main' && answer.labels[0].textContent === 'Answer', 'Choice did not fill the labeled answer');
  form.querySelector('button[type=submit]').click();
  await settled();
  const payload = calls.at(-1)[1];
  check(Object.entries(target).every(([field, value]) => payload[field] === value) && payload.data.text === 'main',
    'Decision submission changed the target or answer');
  check(message.textContent.includes('The request changed') && answer.value === 'main' && !answer.disabled,
    'Stale feedback did not preserve the draft and restore the controls');
  byId('work-view-refresh').click();
  await settled();
  cards.querySelector('button[type=button]').click();
  cards.querySelector('button[type=submit]').click();
  await settled();
  check(!cards.children.length && message.textContent.includes('Decision saved'), 'Decided question stayed in the Inbox');
  byId('work-view-history').click();
  await settled();
  check(message.textContent === 'No recorded decisions.' && byId('work-view-title').textContent === 'Recent decisions',
    'The history view lost its accessible title or empty state');
}

const assets = new URL('../assets/', import.meta.url);
const read = (name) => readFileSync(new URL(name, assets), 'utf8');
const directory = mkdtempSync(join(tmpdir(), 'flock-board-inbox-dom-'));
try {
  const html = read('index.html').replace(/<link\b[^>]*>/g, '').replace('<script type="module" src="/app.js"></script>', `
    <style>${read('style.css')}${read('dialog.css')}${read('board-inbox.css')}</style>
    <script type="module">
      import { mountBoardInbox } from ${JSON.stringify(new URL('board-inbox.js', assets).href)};
      try {
        await (${exercise})(mountBoardInbox);
        document.documentElement.dataset.flockResult = 'pass';
      } catch (error) {
        document.documentElement.dataset.flockResult = error.message;
      }
    </script>`);
  const fixture = join(directory, 'index.html');
  writeFileSync(fixture, html);
  const output = execFileSync(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--allow-file-access-from-files', `--user-data-dir=${directory}`,
    '--virtual-time-budget=3000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
  const result = output.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the DOM checks');
} finally {
  rmSync(directory, { recursive: true, force: true });
}
