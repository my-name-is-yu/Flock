import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

async function exercise(mountBoardInbox) {
  const byId = (id) => document.getElementById(id);
  const check = (value, message) => { if (!value) throw new Error(message); };
  const settled = () => new Promise((resolve) => setTimeout(resolve, 0));
  const button = (label) => [...item.querySelectorAll('button')].find((control) => control.textContent === label);
  const trigger = byId('needs-you');
  const panel = byId('decision-panel');
  const layer = byId('decision-layer');
  const previous = byId('decision-previous');
  const next = byId('decision-next');
  const item = byId('decision-item');
  const message = byId('decision-message');
  const conversation = byId('conversation');
  const editor = byId('message');
  byId('profile-identity').hidden = false;
  byId('composer-container').hidden = false;

  const prompt = '<img data-user-content> branch?';
  const target = { botId: 'release', itemKind: 'user_question', itemId: 2, waitId: 2, taskId: 1,
    targetRevision: 3, targetGeneration: 4 };
  const question = { ...target, botName: 'Release', requestTitle: 'Draft notes', request: 'Draft notes for main',
    prompt, choices: ['main', 'stable'], canDecide: true, conversationEventId: 18 };
  const revised = { ...question, itemId: 5, waitId: 5, targetRevision: 6, targetGeneration: 7,
    prompt: 'Which release branch should we use?', choices: ['main', 'release'] };
  const secret = { ...question, itemKind: 'protected_input', itemId: 3, requestTitle: 'Protected input',
    prompt: 'Supply a value', waitId: 0, taskId: 0, targetRevision: 0, targetGeneration: 0 };
  const review = { ...question, itemKind: 'pr_review', itemId: 4, canDecide: false,
    requestTitle: 'Review a change', prompt: 'Review requested', prTitle: 'Review link', prUrl: 'data:text/plain,ignored' };
  const docs = { ...question, botId: 'docs', botName: 'Docs', itemId: 6, waitId: 6, taskId: 2,
    requestTitle: 'Publish docs', request: 'Publish docs', prompt: 'Which guide?', choices: [] };
  const nextDocs = { ...docs, itemId: 7, waitId: 7, targetGeneration: 5,
    prompt: 'Which examples, and how many?', choices: ['Quickstart', { options: ['1', '2'] }] };

  const calls = [];
  const sources = [];
  let questions = [question, docs, secret, review];
  let finishDocs;
  const choices = () => item.querySelector('.question-choice-form');
  const selected = () => choices().querySelector('input:checked')?.value;
  const sendChoice = () => choices().querySelector('button');
  const submissions = () => calls.filter(([path]) => path === 'inbox/decide').map(([, data]) => data);
  const board = mountBoardInbox({
    async api(path, data) {
      calls.push([path, data]);
      if (path === 'inbox/decide') {
        if (data.waitId === docs.waitId) {
          await new Promise((resolve) => { finishDocs = resolve; });
          questions = questions.filter((entry) => entry !== docs);
        } else if (data.waitId === question.waitId) {
          questions = [revised];
          throw new Error('request_changed');
        } else {
          questions = questions.filter(({ itemId }) => itemId !== data.itemId);
        }
        return { accepted: true };
      }
      if (path.startsWith('inbox/history')) return { decisions: [] };
      if (path !== 'inbox') throw new Error('Unexpected route ' + path);
      return { items: questions };
    },
    sourceAction(entry, close) {
      if (!entry.conversationEventId) return null;
      const source = document.createElement('button');
      source.type = 'button';
      source.textContent = 'Show in conversation';
      source.addEventListener('click', () => { sources.push([entry.botId, entry.conversationEventId]); close(); });
      return source;
    },
    onChange() {},
    onAccepted() {},
    canCompose: () => true,
    focusComposer: () => editor.focus(),
  });
  const open = async (bot, viewer = 'viewer') => {
    board.setContext(bot, viewer);
    board.open(trigger);
    await settled();
  };
  const browse = async (control) => { control.click(); await settled(); };

  board.setContext('release', 'viewer');
  await board.refresh();
  check(layer.hidden && !conversation.inert && !trigger.hidden && message.getAttribute('role') === 'status',
    'The decision layer must start closed with a visible pending count and status region');
  check(!document.querySelector('[data-work-view]') && board.count('release') === 3 && board.count('docs') === 1,
    'Pending counts did not stay scoped to each Bot');
  await browse(trigger);
  check(board.isOpen() && !layer.hidden && conversation.inert && !editor.closest('[inert]') &&
    trigger.getAttribute('aria-expanded') === 'true' && byId(panel.getAttribute('aria-labelledby')).textContent === 'Needs you',
    'The layer did not open inside the conversation with an accessible title and usable composer');
  check(item.textContent.includes(prompt) && !item.textContent.includes(docs.prompt) && !item.querySelector('img,a') &&
    previous.disabled && !next.disabled, 'The first card displayed another Bot, unsafe markup, or invalid navigation');
  check(choices().querySelector('legend').textContent === prompt && sendChoice().disabled && !selected(),
    'The structured question did not start with a labeled, unselected radio group');

  await browse(button('Show in conversation'));
  check(layer.hidden && !conversation.inert && sources.length === 1 && sources[0][0] === 'release' && sources[0][1] === 18,
    'The source shortcut lost the current question event or left the layer open');
  await browse(trigger);
  const main = choices().querySelector('input');
  main.click();
  check(selected() === 'main' && !sendChoice().disabled && !board.composer()?.text && !submissions().length,
    'Choosing an option edited the written answer or sent the decision');
  check(!main.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })),
    'Enter on a radio did not suppress implicit submission');
  await board.refresh();
  check(selected() === 'main', 'Refresh lost the selected answer');

  const write = [...item.querySelectorAll('button')].find((control) => /^Write /.test(control.textContent));
  check(write, 'The structured question lacked a route to write an answer');
  write.click();
  check(document.activeElement === editor && board.composer()?.item.itemId === question.itemId && selected() === 'main',
    'Writing an answer did not focus the separate answer composer');
  const draft = 'Use main after the smoke tests pass.';
  board.edit(draft);
  await browse(next);
  const password = item.querySelector('input[type=password]');
  check(password?.labels[0].textContent === 'Protected value' && !board.composer(),
    'The protected input used the conversation composer instead of a labeled password field');
  password.value = 'fixture secret';
  password.dispatchEvent(new Event('input', { bubbles: true }));
  await browse(next);
  check(item.textContent.includes('View only') && !item.querySelector('form,a') && next.disabled && !previous.disabled,
    'A view-only card showed decision controls, an unsafe link, or invalid navigation');
  await browse(previous);
  check(item.querySelector('input[type=password]')?.value === '', 'The view retained a protected value after leaving its card');
  await browse(previous);
  check(board.composer()?.text === draft && selected() === 'main', 'Cycling decisions lost a written or selected answer');

  await open('docs');
  check(board.composer()?.text === '' && !choices() && next.disabled && previous.disabled,
    'Switching to Docs inherited the prior answer or navigation');
  board.edit('Publish the release checklist.');
  const pendingAnswer = board.submitAnswer();
  await settled();
  await board.refresh();
  check(board.composer()?.busy && submissions().length === 1 && typeof finishDocs === 'function',
    'Refresh made an in-flight answer submittable again');
  await open('release');
  editor.focus();
  check(board.composer()?.text === draft, 'The Docs draft replaced the Release draft');
  finishDocs();
  await pendingAnswer;
  await settled();
  check(board.count('docs') === 0 && board.count('release') === 3 && board.composer()?.text === draft &&
    selected() === 'main' && document.activeElement === editor,
    'Finishing a different Bot’s answer changed this card, focus, or draft');

  questions = [question, nextDocs, secret, review];
  await board.refresh();
  await open('docs');
  check(board.composer()?.text === '' && !choices(), 'A heterogeneous question lost its written answer route or inherited a decided draft');
  board.edit('Publish two Quickstart examples.');
  await board.submitAnswer();
  await settled();
  const mixedAnswer = submissions().at(-1);
  check(mixedAnswer.waitId === nextDocs.waitId && mixedAnswer.data.text === 'Publish two Quickstart examples.' &&
    layer.hidden && trigger.hidden && !conversation.inert, 'The last decision did not close the layer and restore the conversation');

  await open('release');
  check(board.composer()?.text === draft, 'Reopening the card lost its saved draft');
  await board.submitAnswer();
  await settled();
  const payload = submissions().at(-1);
  check(Object.entries(target).every(([field, value]) => payload[field] === value) && payload.data.text === draft,
    'Submitting the written answer changed its exact target or text');
  check(item.textContent.includes(revised.prompt) && board.composer()?.blocked && board.composer()?.text === draft &&
    !selected() && sendChoice().disabled, 'A revised question did not block reuse of the old drafts');
  const attempts = submissions().length;
  await board.submitAnswer();
  check(submissions().length === attempts, 'A stale written answer was submitted before it was reviewed');
  const reuse = button('Use draft for this question');
  check(reuse, 'A revised question lacked a way to review its saved draft');
  choices().querySelector('input[value=release]').click();
  check(!sendChoice().disabled && board.composer()?.blocked && reuse.isConnected,
    'A fresh choice confirmed the stale written draft or was blocked by it');
  reuse.click();
  check(!board.composer()?.blocked && board.composer()?.text === draft && document.activeElement === editor,
    'Explicitly reusing the draft did not restore the written answer');
  sendChoice().click();
  await settled();
  const accepted = submissions().at(-1);
  check(Object.keys(target).every((field) => accepted[field] === revised[field]) && accepted.data.text === 'release' &&
    layer.hidden && board.count('release') === 0, 'The selected answer used the wrong target or stayed pending');

  questions = [{ ...revised, itemId: 9, waitId: 9, targetGeneration: 8 }];
  await board.refresh();
  await browse(trigger);
  check(board.composer()?.text === '' && !selected(), 'A recorded answer carried old drafts to a later question');
  board.edit('A private draft');
  board.setContext('release', 'someone else');
  await board.refresh();
  await browse(trigger);
  check(board.composer()?.text === '', 'A previous viewer’s draft appeared for a different viewer');

  board.close();
  board.openHistory(trigger);
  await settled();
  check(!layer.hidden && byId(panel.getAttribute('aria-labelledby')).textContent === 'Recent decisions' &&
    message.textContent.includes('No recorded decisions') && !board.composer(),
    'The Bot’s decision history lost its scoped title or empty state');
  check(calls.some(([path]) => path === 'inbox/history?bot=release'), 'Recent decisions did not request this Bot’s history');
  byId('decision-close').click();
  check(layer.hidden && !conversation.inert, 'The close action left the conversation disabled');
}
const assets = new URL('../assets/', import.meta.url);
const read = (name) => readFileSync(new URL(name, assets), 'utf8');
const directory = mkdtempSync(join(tmpdir(), 'flock-board-inbox-dom-'));
try {
  const html = read('index.html').replace(/<link\b[^>]*>/g, '').replace('<script type="module" src="/app.js"></script>', `
    <style>${read('style.css')}${read('sidebar.css')}${read('conversation.css')}${read('composer.css')}${read('dialog.css')}${read('board-inbox.css')}</style>
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
