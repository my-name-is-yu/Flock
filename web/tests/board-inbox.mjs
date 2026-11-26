import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { promisify } from 'node:util';

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

  const approval = { ...question, itemKind: 'policy_approval', itemId: 10, status: 'pending',
    action: 'repository.pr', target: 'team/guide', canSaveRule: false };
  const calls = [];
  const sources = [];
  const outcomes = [];
  let questions = [question, docs, secret, review];
  let finishDocs;
  let rejectApproval;
  let readInbox;
  let historySignal;
  let retries = 0;
  const choices = () => item.querySelector('.question-choice-form');
  const selected = () => choices().querySelector('input:checked')?.value;
  const sendChoice = () => choices().querySelector('button');
  const submissions = () => calls.filter(([path]) => path === 'inbox/decide').map(([, data]) => data);
  const board = mountBoardInbox({
    async api(path, data, { signal } = {}) {
      calls.push([path, data]);
      if (path === 'inbox/decide') {
        if (data.itemId === approval.itemId) await new Promise((_, reject) => { rejectApproval = reject; });
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
      if (path.startsWith('inbox/history')) {
        historySignal = signal;
        return { decisions: [] };
      }
      if (path !== 'inbox') throw new Error('Unexpected route ' + path);
      if (readInbox) {
        const read = readInbox;
        readInbox = undefined;
        return read();
      }
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
    onAccepted(entry, result) { outcomes.push({ entry, result }); },
    onRefreshError() { retries++; },
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
  check(board.count('release') === 3 && board.count('docs') === 1,
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
  check(password?.labels[0].textContent === 'Secret value' && !board.composer(),
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
  check(!layer.hidden && byId(panel.getAttribute('aria-labelledby')).textContent === 'History' &&
    message.textContent.includes('Nothing here yet') && !board.composer(),
    'The Bot’s decision history lost its scoped title or empty state');
  check(calls.some(([path]) => path === 'inbox/history?bot=release'), 'History did not request this Bot’s history');
  byId('decision-close').click();
  check(layer.hidden && !conversation.inert, 'The close action left the conversation disabled');
  check(historySignal?.aborted, 'Closing decision history did not abort its fetch signal');

  const denied = (status) => Object.assign(new Error('access_denied'), { status });
  questions = [revised];
  for (const [index, error] of [null, new TypeError('Network unavailable'), denied(403)].entries()) {
    let finishRead;
    readInbox = () => new Promise((resolve, reject) => {
      finishRead = () => error ? reject(error) : resolve({ items: [question] });
    });
    const previousViewer = board.refresh();
    board.setContext('release', `viewer-${index}`);
    await board.refresh();
    await browse(trigger);
    finishRead();
    await previousViewer;
    check(board.count('release') === 1 && item.textContent.includes(revised.prompt) && retries === 0,
      'A response from the previous viewer changed the current Inbox or scheduled a retry');
  }
  for (const status of [401, 403]) {
    await board.refresh();
    board.open(trigger);
    check(!layer.hidden, 'The pending panel must be open before testing authorization denial');
    readInbox = async () => { throw denied(status); };
    await board.refresh();
    check(board.count('release') === 0 && layer.hidden && trigger.hidden && retries === 0,
      `HTTP ${status} did not clear the Inbox and close its pending panel`);
  }

  questions = [approval];
  board.setContext('release', 'approver');
  await board.refresh();
  const inline = board.inlinePolicyCard(approval);
  const approvalActions = (card) => [...card.querySelectorAll('.board-inbox-actions button')].map(
    (control) => [control.value, control.textContent],
  );
  const expectApprovalActions = (entry, expected) => {
    for (const card of [board.inlinePolicyCard(entry), item]) {
      check(JSON.stringify(approvalActions(card)) === JSON.stringify(expected),
        'The inline card and Needs you must show the same authorized approval options');
    }
  };
  board.open(trigger);
  expectApprovalActions(approval, [['allow_once', 'Allow once'], ['deny', 'Deny']]);
  const adminApproval = { ...approval, canSaveRule: true };
  questions = [adminApproval];
  await board.refresh();
  expectApprovalActions(adminApproval, [['allow_once', 'Allow once'], ['always_allow', 'Allow for this Flock'], ['deny', 'Deny']]);
  const staleAdminCard = board.inlinePolicyCard(adminApproval);
  conversation.append(staleAdminCard);
  const expired = { ...approval, status: 'expired', canDecide: false };
  questions = [expired];
  await board.refresh();
  check(approvalActions(board.inlinePolicyCard(expired)).length === 0 && board.itemsFor('release').length === 1,
    'A requester must still see a read-only expired inline card');
  check(board.count('release') === 0 && trigger.hidden && layer.hidden,
    'An expired approval must leave the requester’s Needs you queue');
  const adminExpired = { ...adminApproval, status: 'expired' };
  questions = [adminExpired];
  await board.refresh();
  board.open(trigger);
  expectApprovalActions(adminExpired, [['allow_next_time', 'Allow next time for this Flock']]);
  questions = [approval];
  await board.refresh();
  const beforeRule = submissions().length;
  staleAdminCard.querySelector('button[value=always_allow]').click();
  check(submissions().length === beforeRule, 'A stale admin card could still send a rule decision after permission changed');
  staleAdminCard.remove();
  board.close();
  conversation.append(inline);
  const allowOnce = inline.querySelector('button[value=allow_once]');
  const beforeApproval = outcomes.length;
  allowOnce.click();
  rejectApproval(new Error('request_changed'));
  await settled();
  check(outcomes.length === beforeApproval + 1 && outcomes.at(-1).result.error,
    'The original viewer did not receive their inline approval error');
  allowOnce.click();
  board.setContext('release', 'next-viewer');
  await board.refresh();
  rejectApproval(new Error('request_changed'));
  await settled();
  check(outcomes.length === beforeApproval + 1, 'A previous viewer’s inline approval error reached the new viewer');
}

function liveFixture() {
  const bot = { id: 'release', name: 'Release', model: 'demo', latestIncomingEventId: 0,
    presence: 'waiting', preview: 'Which branch?' };
  const decision = { botId: bot.id, botName: bot.name, itemKind: 'user_question', itemId: 1, waitId: 1,
    taskId: 1, targetRevision: 1, targetGeneration: 1, requestTitle: 'ask:Which branch?', prompt: 'Which branch?', canDecide: true };
  const question = { id: 1, reason: 'user_question', text: decision.prompt, messageId: 2 };
  const review = { ...decision, itemKind: 'pr_review', itemId: 3, waitId: 0, taskId: 4, conversationEventId: 3,
    requestTitle: 'Prepare the release', prTitle: 'Release notes', prompt: 'Review these notes.' };
  const approval = { ...decision, itemKind: 'policy_approval', itemId: 2, taskId: 2, waitId: 2,
    action: 'repository.pr', target: 'team/guide', requestTitle: 'repository.pr · team/guide',
    prompt: JSON.stringify(['git_update_pr', {prNumber: 42, state: 'closed'}]), canDecide: true, canSaveRule: false, status: 'pending' };
  const wait = {id: 2, reason: 'policy_approval', text: approval.requestTitle};
  const stopWait = { id: 9, reason: 'model_failure', text: 'The last attempt failed.' };
  const stopTask = { id: 3, currentRevision: 5, generation: 7, request: 'Check the release', wait: stopWait };
  const limits = { messageBytes: 45, waitAnswerBytes: 33, attachments: 1 };
  const fixture = window.inboxFixture = { bot, decision, review, limits, reads: 0, approvalReady: false, updated: false, failed: false,
    showStop: false, canDecide: true, stopRequests: [], answers: [] };
  window.EventSource = class extends EventTarget {
    constructor(path) {
      super();
      this.path = path;
      fixture.stream = this;
    }
    close() { this.closed = true; }
  };
  window.fetch = async (path, init) => {
    const headers = { 'X-Flock-Viewer': '"viewer"' };
    if (path === '/api/operations' || path === '/api/inbox/decide') {
      const target = JSON.parse(init.body);
      if (target.operation === 'cancel') {
        fixture.stopRequests.push(target);
        return new Promise(resolve => { fixture.finishStop = (status, body) => resolve(Response.json(body, { status })); });
      }
      fixture.answers.push([path, target]);
      return new Promise(resolve => { fixture.finishAnswer = (status, body) => resolve(Response.json(body, { status })); });
    }
    if (path === '/api/bots') return Response.json([bot], { headers });
    if (path.startsWith('/api/view?')) {
      const events = [{id: 4, taskId: 2, time: 1, kind: 'waiting', data: wait}];
      if (fixture.updated) events.push({id: 5, taskId: 2, time: 2, kind: 'wait_closed',
        data: { waitId: 2, resolution: 'allow_once' }});
      const waits = fixture.updated ? [] : [{id: 2, wait}];
      if (!fixture.updated) {
        events.unshift({ id: question.messageId, taskId: decision.taskId, time: 1, kind: 'waiting', data: question },
          { id: review.conversationEventId, taskId: review.taskId, time: 1, kind: 'pr_review_requested',
            data: { title: review.prTitle, text: review.prompt } });
        waits.push({ id: decision.taskId, wait: question });
      }
      if (fixture.showStop) {
        waits.push(stopTask);
        events.push({ id: 6, taskId: stopTask.id, time: 3, kind: 'waiting', data: stopWait });
      }
      if (fixture.question) {
        const { taskId, waitId, targetRevision, targetGeneration, request, prompt, choices } = fixture.question;
        const wait = { id: waitId, reason: 'user_question', messageId: 7, text: prompt, choices };
        waits.push({ id: taskId, currentRevision: targetRevision, generation: targetGeneration, request, wait });
        events.push({ id: wait.messageId, taskId, time: 4, kind: 'waiting', data: wait });
      }
      return Response.json({ bot, events, olderBefore: 0, presence: 'idle', sessionTasks: [],
        waits, historyCursor: 0, sessionCursor: 0, canDecide: fixture.canDecide, composerPerson: 'viewer', limits }, { headers });
    }
    if (path === '/api/inbox') {
      fixture.reads++;
      if (fixture.updated && !fixture.failed) {
        fixture.failed = true;
        return Response.json({ error: 'server_error' }, { status: 503 });
      }
      if (fixture.question) return Response.json({ items: [fixture.question] });
      const items = fixture.approvalReady ? [decision, review, approval] : [decision, review];
      return Response.json({ items: fixture.updated ? [] : items });
    }
    throw new Error('Unexpected route ' + path);
  };
}

async function exerciseLiveRetry() {
  const fixture = window.inboxFixture;
  const trigger = document.getElementById('needs-you');
  const panel = document.getElementById('decision-layer');
  async function waitFor(predicate, error) {
    const deadline = performance.now() + 3500;
    while (!predicate()) {
      if (performance.now() >= deadline) throw new Error(error);
      await new Promise(resolve => setTimeout(resolve, 25));
    }
  }
  await waitFor(() => !trigger.hidden && fixture.stream?.path === '/api/events?bot=release',
    'The Inbox did not load on the Bot stream');
  const badge = document.querySelector('#bot-list .decision-count');
  const preview = document.querySelector('#bot-list .bot-preview');
  if (badge.hidden || badge.textContent !== '2' || preview.textContent !== fixture.bot.preview ||
      !preview.querySelector('.presence-dot').hidden)
    throw new Error('The Bot row did not show the last message and a single waiting indicator');
  const { offsetWidth: width, offsetHeight: height } = badge;
  if (width < 24 || height < 24) throw new Error('The count button is smaller than 24 × 24 pixels');
  const stream = fixture.stream;
  await waitFor(() => document.getElementById('event-4'), 'The pending wait did not appear');
  const byId = (id) => document.getElementById(id);
  const { messageBytes, waitAnswerBytes, attachments } = fixture.limits;
  const message = byId('message');
  const send = byId('send');
  const notice = byId('notice');
  const editMessage = (value) => {
    message.value = value;
    message.dispatchEvent(new Event('input', { bubbles: true }));
  };
  const expectComposerLimit = (maxBytes) => {
    const atLimit = '界'.repeat(Math.floor(maxBytes / 3)) + 'x'.repeat(maxBytes % 3);
    editMessage(atLimit);
    if (send.disabled) throw new Error(`The composer rejected ${maxBytes} bytes at its limit`);
    editMessage(atLimit + 'x');
    if (!send.disabled || !notice.textContent.includes(`${maxBytes} bytes`))
      throw new Error(`The composer ignored the ${maxBytes}-byte limit from the view`);
    editMessage('');
  };
  expectComposerLimit(messageBytes);
  const canvas = new OffscreenCanvas(1, 1);
  canvas.getContext('2d').fillRect(0, 0, 1, 1);
  const screenshot = await canvas.convertToBlob();
  const clipboardData = new DataTransfer();
  for (let index = 0; index <= attachments; index++) {
    clipboardData.items.add(new File([screenshot], `Screenshot-${index}.png`, { type: screenshot.type }));
  }
  message.dispatchEvent(new ClipboardEvent('paste', { clipboardData, bubbles: true, cancelable: true }));
  const previews = byId('screenshot-previews');
  await waitFor(() => previews.childElementCount && byId('attach-screenshots').getAttribute('aria-disabled') === 'false',
    'Pasted screenshots did not finish staging');
  if (previews.childElementCount !== attachments || !notice.textContent.includes(`Attach up to ${attachments}`))
    throw new Error('The composer ignored the attachment limit from the view');
  previews.querySelector('.remove-screenshot').click();

  const expectComposer = ({ inbox = false, label, summary, placeholder, send }) => {
    const chip = byId(inbox ? 'decision-reply-context' : 'reply-context');
    const target = chip.querySelector('.reply-summary');
    const field = byId('message');
    if (chip.hidden || !byId(inbox ? 'reply-context' : 'decision-reply-context').hidden ||
        chip.querySelector('.reply-label').textContent !== label || target.textContent !== summary ||
        field.placeholder !== placeholder || field.labels[0].textContent !== placeholder ||
        field.getAttribute('aria-describedby') !== `${target.id} notice` || byId('send').getAttribute('aria-label') !== send)
      throw new Error(`The ${inbox ? 'Inbox' : 'conversation'} composer did not present ${label} ${summary}`);
  };
  const answerCopy = { label: 'Answering', summary: fixture.decision.prompt, placeholder: 'Write your answer', send: 'Send answer' };
  const reviewCopy = { label: 'Replying to', summary: fixture.review.prTitle, placeholder: 'Reply to this review', send: 'Send reply' };
  trigger.click();
  const write = panel.querySelector('button.question-reply');
  if (write?.textContent !== 'Write an answer') throw new Error('The Inbox question did not offer to write an answer');
  write.click();
  expectComposer({ ...answerCopy, inbox: true });
  expectComposerLimit(waitAnswerBytes);
  const prompt = fixture.decision.prompt;
  fixture.decision.prompt = '';
  stream.dispatchEvent(new MessageEvent('change', { data: '{"list":true,"bot":"release"}' }));
  await waitFor(() => byId('decision-reply-target').textContent === `Question from ${fixture.bot.name}`,
    'The Inbox exposed the task request when the question text was missing');
  fixture.decision.prompt = prompt;
  byId('decision-next').click();
  expectComposer({ ...reviewCopy, inbox: true });
  byId('decision-close').click();
  const writeInline = document.querySelector('#event-2 button.question-reply');
  if (writeInline?.textContent !== 'Write an answer') throw new Error('The conversation question did not offer to write an answer');
  writeInline.click();
  expectComposer(answerCopy);
  expectComposerLimit(waitAnswerBytes);
  document.querySelector('#event-2 button[aria-label="Answer this question"]').click();
  expectComposer(answerCopy);
  document.querySelector('#event-3 button[aria-label="Reply to this review"]').click();
  expectComposer(reviewCopy);
  byId('cancel-reply').click();
  if (byId('compose-label').textContent !== 'Message' || byId('message').getAttribute('aria-describedby') !== 'notice')
    throw new Error('Canceling the reply retained the reply composer copy');

  const reads = fixture.reads;
  fixture.approvalReady = true;
  stream.dispatchEvent(new Event('ready'));
  await waitFor(() => fixture.reads > reads, 'The ready stream did not catch up');
  const approvalCard = () => document.querySelector('#event-4.policy-inline-card');
  await waitFor(approvalCard, 'A late Inbox response did not replace the first waiting event with its approval');
  if (document.querySelectorAll('.policy-inline-card').length !== 1 || !approvalCard().textContent.includes('Pull request: 42') ||
      approvalCard().querySelector('button[value=always_allow]') ||
      ![...approvalCard().querySelectorAll('button')].some(button => button.textContent === 'Allow once'))
    throw new Error('The approval card duplicated the request or displayed the wrong action');
  const review = [...document.querySelectorAll('#wait-list .wait-row')].find(row => row.textContent.includes('Approval needed'));
  if (!review) throw new Error('The approval disappeared from Needs you');
  review.click();
  if (panel.hidden || !panel.textContent.includes('Pull request: 42') || !panel.textContent.includes('State: closed') ||
      panel.querySelector('button[value=always_allow]') ||
      ![...panel.querySelectorAll('button')].some(button => button.textContent === 'Allow once'))
    throw new Error('The wait did not open the exact approval with its requested change');
  fixture.updated = true;
  stream.dispatchEvent(new MessageEvent('change', { data: '{"list":true,"bot":"release"}' }));
  await waitFor(() => fixture.failed, 'The live invalidation did not fetch the Inbox');
  if (trigger.hidden || panel.hidden) throw new Error('A transient Inbox failure cleared the pending decision');
  await waitFor(() => trigger.hidden && panel.hidden, 'The Inbox stayed stale with a healthy stream after HTTP 503');
  if (!badge.hidden || preview.textContent !== 'Waiting for you' || preview.querySelector('.presence-dot').hidden)
    throw new Error('The waiting Bot did not restore its status after losing its count');
  const resolution = document.querySelector('#event-4 .msg-resolution');
  if (approvalCard() || !resolution?.textContent.includes('Allowed once'))
    throw new Error('Resolving the approval did not restore its waiting event and decision');
  if (stream.closed || fixture.stream !== stream) throw new Error('The Inbox recovered by reconnecting its event stream');

  const refreshView = () => stream.dispatchEvent(new MessageEvent('change', { data: '{"bot":"release"}' }));
  const stop = () => document.querySelector('#detail .wait-cancel');
  const confirmStop = () => document.querySelector('#detail .wait-stop-confirm .danger');
  const openStop = () => document.querySelector('#event-6 .question-actions button');
  fixture.showStop = true;
  fixture.bot.name = 'Release $$ $&';
  refreshView();
  await waitFor(openStop, 'The cancellable wait did not arrive');
  openStop().click();
  const detailTitle = document.getElementById('detail-title');
  if (detailTitle.textContent !== 'Release $$ $& got stuck') throw new Error('The Bot name was interpreted as a replacement pattern');
  fixture.bot.name = 'Recovery';
  refreshView();
  await waitFor(() => detailTitle.textContent === 'Recovery got stuck', 'An open wait dialog kept the old Bot name');
  stop().click();
  const keep = document.querySelector('#detail .wait-stop-confirm .quiet');
  if (document.activeElement !== keep || fixture.stopRequests.length)
    throw new Error('Opening the stop confirmation changed the request or lost focus');
  keep.click();
  if (document.activeElement !== stop() || confirmStop()) throw new Error('Dismissing the confirmation did not restore the wait actions');
  stop().click();
  const detail = document.getElementById('detail');
  if (detail.dispatchEvent(new Event('cancel', { cancelable: true })) || !detail.open ||
      confirmStop() || document.activeElement !== stop())
    throw new Error('Escape closed the request detail or submitted the stop');
  stop().click();
  fixture.canDecide = false;
  refreshView();
  await waitFor(() => !confirmStop() && !stop(), 'Permission loss did not clear the stop confirmation');
  if (fixture.stopRequests.length) throw new Error('Permission loss submitted a stop');
  fixture.canDecide = true;
  refreshView();
  await waitFor(stop, 'The stop action did not return after permission was restored');
  stop().click();
  confirmStop().click();
  confirmStop().click();
  const [target] = fixture.stopRequests;
  if (fixture.stopRequests.length !== 1 || target.botId !== 'release' || target.operation !== 'cancel' ||
      target.taskId !== 3 || target.waitId !== 9 || target.targetRevision !== 5 || target.targetGeneration !== 7)
    throw new Error('The stop was submitted twice or lost its exact target');
  fixture.finishStop(409, { error: 'request_changed' });
  await waitFor(() => confirmStop() && !confirmStop().disabled, 'A failed stop left the confirmation disabled');
  document.getElementById('close-detail').click();
  fixture.showStop = false;

  const first = fixture.question = { botId: 'release', taskId: 4, waitId: 10, itemId: 10,
    itemKind: 'user_question', targetRevision: 6, targetGeneration: 8, canDecide: true,
    requestTitle: 'Draft notes', request: 'Draft notes', prompt: 'Which branch?', choices: ['main', 'stable'] };
  const timelineForm = () => document.querySelector('#event-7 .question-choice-form');
  const inboxForm = () => panel.querySelector('.question-choice-form');
  const confirm = (form) => form.querySelector('button');
  const chosen = (form) => form.querySelector('input:checked')?.value;
  const closeInbox = () => document.getElementById('decision-close').click();
  const updateQuestion = () => stream.dispatchEvent(new MessageEvent('change', { data: '{"list":true,"bot":"release"}' }));

  updateQuestion();
  await waitFor(() => timelineForm() && !trigger.hidden, 'The shared question did not reach both views');
  timelineForm().querySelector('input[value=stable]').click();
  trigger.click();
  if (chosen(inboxForm()) !== 'stable') throw new Error('The Inbox lost the choice from the conversation');
  const draft = document.getElementById('message');
  draft.value = 'Use the maintenance branch.';
  draft.dispatchEvent(new Event('input', { bubbles: true }));
  closeInbox();

  confirm(timelineForm()).click();
  const [path, payload] = fixture.answers[0];
  if (path !== '/api/operations' || payload.text !== 'stable' || payload.operation !== 'answer' ||
      !['botId', 'taskId', 'waitId', 'targetRevision', 'targetGeneration'].every(field => payload[field] === first[field]))
    throw new Error('The conversation answer lost the exact wait target');
  trigger.click();
  if (!confirm(inboxForm()).disabled || !draft.readOnly || chosen(inboxForm()) !== 'stable')
    throw new Error('The Inbox did not share the pending conversation answer');
  confirm(inboxForm()).click();
  if (fixture.answers.length !== 1) throw new Error('Both views submitted the same pending wait');
  fixture.finishAnswer(200, { accepted: false });
  await waitFor(() => !confirm(inboxForm()).disabled && !draft.readOnly, 'An unconfirmed answer could not be retried');
  if (chosen(inboxForm()) !== 'stable' || draft.value !== 'Use the maintenance branch.')
    throw new Error('An unconfirmed answer discarded the drafts');

  const previousForm = timelineForm();
  confirm(inboxForm()).click();
  const [retryPath, retry] = fixture.answers[1];
  if (retryPath !== '/api/inbox/decide' || retry.data.text !== 'stable' || retry.decision !== 'answer' ||
      !['botId', 'taskId', 'itemKind', 'itemId', 'waitId', 'targetRevision', 'targetGeneration'].every(field => retry[field] === first[field]))
    throw new Error('The Inbox retry changed the decision or its exact target');
  closeInbox();
  if (!confirm(timelineForm()).disabled) throw new Error('The conversation did not share the pending Inbox answer');
  fixture.question = { ...first, targetRevision: 7, targetGeneration: 9, prompt: 'Which branch for the new request?' };
  fixture.finishAnswer(409, { error: 'request_changed' });
  trigger.click();
  await waitFor(() => inboxForm()?.textContent && panel.textContent.includes(fixture.question.prompt),
    'The Inbox never refreshed the revised question');
  if (chosen(inboxForm()) || !confirm(inboxForm()).disabled || !document.getElementById('send').disabled ||
      draft.value !== 'Use the maintenance branch.')
    throw new Error('The revised question reused a stale selection or lost the written draft');
  closeInbox();
  await waitFor(() => timelineForm()?.querySelector('legend').textContent === fixture.question.prompt,
    'The conversation did not catch up to the revised question');
  if (chosen(timelineForm()) || !confirm(timelineForm()).disabled)
    throw new Error('The conversation reused a selection from a previous revision');
  previousForm.dispatchEvent(new Event('submit', { cancelable: true }));
  if (fixture.answers.length !== 2) throw new Error('The conversation submitted an older wait target');
  timelineForm().querySelector('input[value=main]').click();
  confirm(timelineForm()).click();
  const [, final] = fixture.answers[2];
  if (final.text !== 'main' || final.targetRevision !== 7 || final.targetGeneration !== 9)
    throw new Error('The new decision did not use the revised target and choice');
  fixture.question = null;
  fixture.finishAnswer(200, { accepted: true });
  await waitFor(() => !timelineForm() && trigger.hidden, 'Acceptance did not clear both decision entry points');
}

const assets = new URL('../assets/', import.meta.url);
const read = (name) => readFile(new URL(name, assets), 'utf8');
const index = await read('index.html');
const withScript = (html, script) => html.replace('<script type="module" src="/app.js"></script>', () => script);
const boardPage = async () => withScript(index.replace(/<link\b[^>]*>/g, ''), `
  <style>${(await Promise.all(['style.css', 'sidebar.css', 'conversation.css', 'composer.css', 'dialog.css',
    'board-inbox.css'].map(read))).join('')}</style>
  <script type="module">
    import { mountBoardInbox } from ${JSON.stringify(new URL('board-inbox.js', assets).href)};
    try {
      await (${exercise})(mountBoardInbox);
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script>`);
const livePage = withScript(index, `
  <script>
    const reportBrowserResult = window.fetch.bind(window);
    (${liveFixture})();
  </script>
  <script type="module">
    try {
      await import('/app.js');
      await (${exerciseLiveRetry})();
      await reportBrowserResult('/result?passed');
    } catch (error) {
      await reportBrowserResult('/result?error=' + encodeURIComponent(error.message));
    }
  </script>`);
const contentTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
async function testLive(browserArgs) {
  const server = createServer(async (request, response) => {
    const { pathname, searchParams } = new URL(request.url, 'http://localhost');
    if (pathname === '/result') {
      response.writeHead(204).end();
      server.emit('browserResult', searchParams.get('error'));
      return;
    }
    if (pathname === '/') return response.writeHead(200, { 'Content-Type': 'text/html' }).end(livePage);
    try {
      const asset = await read('.' + pathname);
      response.writeHead(200, { 'Content-Type': contentTypes[extname(pathname)] ?? 'application/octet-stream' }).end(asset);
    } catch {
      response.writeHead(404).end();
    }
  });
  let chrome;
  try {
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const url = `http://127.0.0.1:${server.address().port}/?bot=release`;
    chrome = spawn(process.argv[2], [...browserArgs, url], { stdio: 'ignore' });
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
  }
}

const directory = await mkdtemp(join(tmpdir(), 'flock-board-inbox-dom-'));
const browserArgs = ['--headless', '--disable-gpu', '--no-first-run', '--disable-background-networking', `--user-data-dir=${directory}`];
try {
  if (process.argv[3] === 'live') {
    await testLive(browserArgs);
  } else {
    const fixture = join(directory, 'index.html');
    await writeFile(fixture, await boardPage());
    const { stdout } = await promisify(execFile)(process.argv[2], [...browserArgs, '--allow-file-access-from-files',
      '--virtual-time-budget=3000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
    const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
    if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the DOM checks');
  }
} finally {
  await rm(directory, { recursive: true, force: true });
}
