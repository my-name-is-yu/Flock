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
  const bots = byId('work-view-bot');
  const refresh = byId('work-view-refresh');
  const cards = byId('work-view-items');
  const message = byId('work-view-message');
  const text = '<img data-user-content> branch?';
  const target = { botId: 'release', itemKind: 'user_question', itemId: 2, waitId: 2, taskId: 1,
    targetRevision: 3, targetGeneration: 4 };
  const question = { ...target, botName: 'Release', requestTitle: 'Draft notes', request: 'Draft notes for main',
    prompt: text, choices: ['main'], canDecide: true };
  const revised = { ...question, itemId: 5, waitId: 5, targetRevision: 6, targetGeneration: 7,
    prompt: 'Which release branch should we use?', choices: ['release'] };
  const secret = { ...question, itemKind: 'protected_input', itemId: 3, requestTitle: 'Protected input' };
  const review = { ...question, itemKind: 'pr_review', itemId: 4, canDecide: false,
    prTitle: 'Review link', prUrl: 'data:text/plain,ignored' };
  const docs = { ...question, botId: 'docs', botName: 'Docs', itemId: 6, waitId: 6, taskId: 2,
    requestTitle: 'Publish docs', request: 'Publish docs', prompt: 'Which guide?', choices: [] };
  const nextDocs = { ...docs, itemId: 7, waitId: 7, targetGeneration: 5, prompt: 'Which examples?' };
  const calls = [];
  let questions = [question, docs, secret, review];
  let recordDocs;
  const answer = () => cards.querySelector('textarea');
  const type = (input, value) => {
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  };
  const filter = (bot) => {
    bots.value = bot;
    bots.dispatchEvent(new Event('change'));
  };
  mountBoardInbox(async (path, data) => {
    calls.push([path, data]);
    if (path === 'bots') return [{ id: 'release', name: 'Release' }, { id: 'docs', name: 'Docs' }];
    if (path.startsWith('board')) return { tasks: [] };
    if (path === 'inbox/decide') {
      if (data.waitId === docs.waitId) {
        await new Promise((resolve) => { recordDocs = resolve; });
        questions = questions.filter((item) => item !== docs);
        return { accepted: true };
      }
      if (data.waitId === question.waitId) {
        questions = [revised];
        throw new Error('request_changed');
      }
      questions = [];
      return { accepted: true };
    }
    if (path === 'inbox/history') return { decisions: [] };
    const bot = new URLSearchParams(path.split('?')[1]).get('bot');
    return { items: questions.filter((item) => !bot || item.botId === bot) };
  });

  check(!dialog.open && message.getAttribute('role') === 'status', 'Missing closed dialog or live status');
  document.querySelector('[data-work-view="board"]').click();
  await settled();
  check(dialog.open && byId(dialog.getAttribute('aria-labelledby')).textContent === 'Task board' && !filters.hidden,
    'The board lost its accessible title or status filter');
  bots.value = 'release';
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
  check(answer().value === 'main' && answer().labels[0].textContent === 'Answer', 'Choice did not fill the labeled answer');
  refresh.click();
  await settled();
  check(answer().value === 'main', 'Refresh lost the selected answer');
  const draft = 'Use main after the smoke tests pass.';
  type(answer(), draft);
  type(cards.querySelector('input[type=password]'), 'fixture secret');
  filter('docs');
  await settled();
  check(answer().value === '', 'The Release draft appeared on the Docs card');
  type(answer(), 'Publish the release checklist.');
  byId('work-view-close').click();
  await settled();
  document.querySelector('[data-work-view="inbox"]').click();
  await settled();
  check(answer().value === draft && cards.querySelectorAll('textarea')[1].value === 'Publish the release checklist.',
    'Filtering and reopening lost the answer drafts');
  check(cards.querySelector('input[type=password]').value === '', 'The draft retained a protected value');

  cards.querySelectorAll('textarea')[1].form.querySelector('button[type=submit]').click();
  refresh.click();
  await settled();
  check(cards.querySelectorAll('textarea')[1].form.querySelector('button[type=submit]').disabled,
    'Refreshing made an in-flight answer submit again');
  answer().focus();
  answer().setSelectionRange(4, 8);
  recordDocs();
  await settled();
  check(answer().value === draft && !cards.textContent.includes(docs.prompt) && message.textContent.includes('Decision saved'),
    'Saving the Docs answer lost the Release draft or left Docs pending');
  check(document.activeElement === answer() && answer().selectionStart === 4 && answer().selectionEnd === 8,
    'Saving the Docs answer moved focus or selection out of the Release editor');
  questions = [question, nextDocs, secret, review];
  refresh.click();
  await settled();
  check(cards.querySelectorAll('textarea')[1].value === '', 'The recorded Docs draft appeared on a new Docs question');

  let send = answer().form.querySelector('button[type=submit]');
  send.focus();
  send.click();
  await settled();
  const payload = calls.filter(([route]) => route === 'inbox/decide').at(-1)[1];
  check(Object.entries(target).every(([field, value]) => payload[field] === value) && payload.data.text === draft,
    'Decision submission changed the target or answer');
  send = answer().form.querySelector('button[type=submit]');
  check(cards.textContent.includes(revised.prompt) && cards.textContent.includes('The question changed') && send.disabled,
    'The updated question did not require review before reusing the draft');
  check(document.activeElement === answer() && answer().value === draft && !answer().disabled,
    'The changed request lost the draft or editor focus');
  const reuse = [...answer().form.querySelectorAll('button')].find(({ textContent }) => textContent === 'Use draft for this question');
  reuse.click();
  check(!send.disabled && document.activeElement === answer(), 'Reviewing the draft did not restore the answer controls');
  send.click();
  await settled();
  const accepted = calls.filter(([route]) => route === 'inbox/decide').at(-1)[1];
  check(Object.keys(target).every((field) => accepted[field] === revised[field]) && accepted.data.text === draft,
    'The reviewed draft did not use the updated decision target');
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
