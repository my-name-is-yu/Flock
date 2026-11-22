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
  const bots = byId('work-view-bot');
  const refresh = byId('work-view-refresh');
  const cards = byId('work-view-items');
  const message = byId('work-view-message');
  const text = '<img data-user-content> branch?';
  const target = { botId: 'release', itemKind: 'user_question', itemId: 2, waitId: 2, taskId: 1,
    targetRevision: 3, targetGeneration: 4 };
  const question = { ...target, botName: 'Release', requestTitle: 'Draft notes', request: 'Draft notes for main',
    prompt: text, choices: ['main', 'stable'], canDecide: true, conversationEventId: 18 };
  const revised = { ...question, itemId: 5, waitId: 5, targetRevision: 6, targetGeneration: 7,
    prompt: 'Which release branch should we use?', choices: ['main', 'release'] };
  const secret = { ...question, itemKind: 'protected_input', itemId: 3, requestTitle: 'Protected input' };
  const review = { ...question, itemKind: 'pr_review', itemId: 4, canDecide: false,
    prTitle: 'Review link', prUrl: 'data:text/plain,ignored' };
  const docs = { ...question, botId: 'docs', botName: 'Docs', itemId: 6, waitId: 6, taskId: 2,
    requestTitle: 'Publish docs', request: 'Publish docs', prompt: 'Which guide?', choices: [] };
  const nextDocs = { ...docs, itemId: 7, waitId: 7, targetGeneration: 5,
    prompt: 'Which examples, and how many?', choices: ['Quickstart', { options: ['1', '2'] }] };
  const calls = [];
  const sources = [];
  let questions = [question, docs, secret, review];
  let recordDocs;
  const answer = () => cards.querySelector('textarea');
  const choices = () => cards.querySelector('.question-choice-form');
  const selected = () => choices().querySelector('input:checked')?.value;
  const sendChoice = () => choices().querySelector('button');
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
      if (data.waitId === nextDocs.waitId) {
        questions = questions.filter((item) => item !== nextDocs);
        return { accepted: true };
      }
      questions = [];
      return { accepted: true };
    }
    if (path === 'inbox/history') return { decisions: [] };
    const bot = new URLSearchParams(path.split('?')[1]).get('bot');
    return { items: questions.filter((item) => !bot || item.botId === bot) };
  }, (bot, eventId, close) => {
    const button = document.createElement('button');
    button.textContent = 'Show in conversation';
    button.addEventListener('click', () => { sources.push([bot.id, eventId]); close(); });
    return button;
  });

  check(!dialog.open && message.getAttribute('role') === 'status', 'Missing closed dialog or live status');
  check(!document.querySelector('[data-work-view="board"]') && !byId('work-view-status'),
    'The sidebar still offers the Task board');
  document.querySelector('[data-work-view="inbox"]').click();
  await settled();
  check(dialog.open && byId(dialog.getAttribute('aria-labelledby')).textContent === 'Decision inbox',
    'The inbox lost its accessible title');
  check(cards.textContent.includes(text) && !cards.querySelector('img,a'), 'User text or an unsafe URL entered the DOM');
  check(!cards.lastElementChild.querySelector('form') && cards.lastElementChild.textContent.includes('View only'),
    'A view-only item showed decision controls');
  const password = cards.querySelector('input[type=password]');
  check(password?.labels[0].textContent === 'Protected value', 'The protected value is not a labeled password field');
  const release = cards.firstElementChild;
  check(choices().querySelector('legend').textContent === text && sendChoice().disabled &&
    answer().form.hidden && !selected(), 'The structured question did not start with a labeled, unselected radio group');
  release.querySelector('button').click();
  await settled();
  check(!dialog.open && sources.length === 1 && sources[0][0] === 'release' && sources[0][1] === 18,
    'Show in conversation did not preserve the question source');
  document.querySelector('[data-work-view="inbox"]').click();
  await settled();
  const main = choices().querySelector('input');
  main.click();
  check(selected() === 'main' && !sendChoice().disabled && !answer().value &&
    !calls.some(([path]) => path === 'inbox/decide'), 'Choosing a radio edited the written draft or sent a decision');
  check(!main.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })),
    'Enter on a radio did not suppress implicit submission');
  refresh.click();
  await settled();
  check(selected() === 'main' && answer().form.hidden, 'Refresh lost the selected answer or opened the editor');
  const write = cards.querySelector('.question-reply');
  write.click();
  check(!answer().form.hidden && document.activeElement === answer() && answer().labels[0].textContent === 'Answer' &&
    write.textContent === 'Write a reply' && selected() === 'main', 'Write a reply did not focus the separate answer editor');
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
  check(answer().value === draft && !answer().form.hidden && selected() === 'main' &&
    cards.querySelectorAll('textarea')[1].value === 'Publish the release checklist.',
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
  check(answer().value === draft && selected() === 'main' && !cards.textContent.includes(docs.prompt) &&
    message.textContent.includes('Decision saved'),
    'Saving the Docs answer lost the Release draft or left Docs pending');
  check(document.activeElement === answer() && answer().selectionStart === 4 && answer().selectionEnd === 8,
    'Saving the Docs answer moved focus or selection out of the Release editor');
  questions = [question, nextDocs, secret, review];
  refresh.click();
  await settled();
  const mixed = cards.children[1];
  check(mixed.querySelector('textarea').value === '' && !mixed.querySelector('form').hidden &&
    !mixed.querySelector('input[type=radio]'), 'A heterogeneous question lost its free-text route or inherited the old draft');
  type(mixed.querySelector('textarea'), 'Publish two Quickstart examples.');
  mixed.querySelector('button[type=submit]').click();
  await settled();
  const mixedAnswer = calls.filter(([path]) => path === 'inbox/decide').at(-1)[1];
  check(mixedAnswer.waitId === nextDocs.waitId && mixedAnswer.data.text === 'Publish two Quickstart examples.' &&
    !mixed.isConnected && answer().value === draft, 'The heterogeneous question could not send a written answer');

  let send = answer().form.querySelector('button[type=submit]');
  send.focus();
  send.click();
  await settled();
  const payload = calls.filter(([route]) => route === 'inbox/decide').at(-1)[1];
  check(Object.entries(target).every(([field, value]) => payload[field] === value) && payload.data.text === draft,
    'Decision submission changed the target or answer');
  send = answer().form.querySelector('button[type=submit]');
  check(cards.textContent.includes(revised.prompt) && cards.textContent.includes('The question changed') && send.disabled &&
    !selected() && sendChoice().disabled,
    'The updated question did not require review before reusing the draft');
  check(document.activeElement === answer() && answer().value === draft && !answer().disabled,
    'The changed request lost the draft or editor focus');
  const reuse = [...answer().form.querySelectorAll('button')].find(({ textContent }) => textContent === 'Use draft for this question');
  check(reuse && send.disabled, 'An unchanged old draft must still be reviewed');
  const choice = choices().querySelector('input[value=release]');
  choice.click();
  check(!sendChoice().disabled && send.disabled && reuse.isConnected && answer().value === draft,
    'A fresh choice was blocked by or wrongly confirmed the old written draft');
  reuse.click();
  check(!send.disabled && !reuse.isConnected && answer().value === draft && document.activeElement === answer(),
    'Explicitly reusing the draft did not restore the written answer');
  sendChoice().click();
  await settled();
  const accepted = calls.filter(([route]) => route === 'inbox/decide').at(-1)[1];
  check(Object.keys(target).every((field) => accepted[field] === revised[field]) && accepted.data.text === 'release',
    'The selected answer did not use the updated decision target');
  check(!cards.children.length && message.textContent.includes('Decision saved'), 'Decided question stayed in the Inbox');
  questions = [{ ...revised, itemId: 9, waitId: 9, targetGeneration: 8 }];
  refresh.click();
  await settled();
  check(!selected() && answer().form.hidden && !answer().value, 'A recorded answer left drafts in the next question');
  byId('work-view-history').click();
  await settled();
  check(message.textContent === 'No recorded decisions.' && byId('work-view-title').textContent === 'Recent decisions',
    'The history view lost its accessible title or empty state');
  filter('release');
  await settled();
  check(calls.at(-1)[0] === 'inbox/history?bot=release', 'The Bot filter did not narrow the request');
}

const assets = new URL('../assets/', import.meta.url);
const read = (name) => readFileSync(new URL(name, assets), 'utf8');
const directory = mkdtempSync(join(tmpdir(), 'flock-board-inbox-dom-'));
try {
  const html = read('index.html').replace(/<link\b[^>]*>/g, '').replace('<script type="module" src="/app.js"></script>', `
    <style>${read('style.css')}${read('dialog.css')}${read('conversation.css')}${read('board-inbox.css')}</style>
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
