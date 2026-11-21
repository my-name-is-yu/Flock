import { showAvatar } from '/avatars.js';

const byId = (id) => document.getElementById(id);
const elements = Object.fromEntries([
  'navigation', 'pinned', 'bot-list', 'wait-list', 'wait-count', 'session-list', 'more-sessions',
  'connection', 'connection-text', 'open-navigation', 'close-navigation', 'profile', 'profile-avatar',
  'profile-name', 'profile-status', 'profile-placeholder', 'settings-button', 'conversation', 'older',
  'timeline', 'composer-container', 'composer', 'reply-context', 'reply-question', 'cancel-reply',
  'reply-label', 'compose-label', 'message', 'send', 'notice', 'detail', 'detail-title', 'detail-body', 'close-detail',
  'theme-switch',
].map((id) => [id, byId(id)]));
const workspace = document.querySelector('.workspace');
const narrow = matchMedia('(max-width: 700px)');
const clock = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
const dayFormat = new Intl.DateTimeFormat('en', { year: 'numeric', month: 'long', day: 'numeric' });
const fullTime = new Intl.DateTimeFormat('en', { dateStyle: 'medium', timeStyle: 'short', hourCycle: 'h23' });
const byteLength = (text) => new TextEncoder().encode(text).length;
const stateLabels = {
  queued: 'Queued', deciding: 'Evaluating', sending: 'Sending', running: 'Working', waiting: 'Waiting for input',
  cancelling: 'Cancelling', stopping: 'Confirming stop', completed: 'Complete', cancelled: 'Cancelled', ended: 'Ended',
  idle: 'Idle', thinking: 'Thinking', working: 'Session in progress', peer_waiting: 'Waiting for a Bot',
};
const peerMessageLabels = {
  request: 'Request', question: 'Question', answer: 'Answer', result: 'Result', cancelled: 'Cancelled', failed: 'Failed',
};
const waitLabels = {
  user_question: ['Question from the Bot', 'The Bot is waiting for your answer.'],
  budget: ['Approval for more work', 'The task reached its additional run limit. You can approve one more run.'],
  model_failure: ['Retry decision', 'The model failed to make a decision after several attempts. You can retry.'],
  t3_input: ['Action in T3', 'Open the assigned thread in T3 to approve or answer.'],
  deadline: ['Deadline extension', 'The session reached its deadline. You can set a new one.'],
  unknown_delivery: ['Delivery reconciliation', 'Flock could not confirm whether T3 received the task or answer.'],
  auth: ['T3 authentication', 'Sign in to T3 again, then reconcile.'],
  settings: ['T3 execution settings', 'Check the T3 connection and execution settings, then reconcile.'],
};
const closedLabels = {
  answer: 'Answered', run_finished: 'Session ended', cancel: 'Task cancelled',
  session_question: 'The session updated the question',
};
const errors = {
  invalid_text: 'Check the message and its length.', invalid_deadline: 'Set a deadline in the future.',
  intake_settings: 'Flock could not accept the request. Check the Bot’s execution settings.',
  message_conflict: 'A message was already recorded for this submission. Check the conversation.',
  operation_conflict: 'Flock already processed this action or the pending request changed.',
  bot_not_found: 'Bot not found.',
};

function saved(key, fallback) {
  try { return JSON.parse(localStorage.getItem(key)) ?? fallback; }
  catch { return fallback; }
}
function save(key, value) {
  try { localStorage.setItem(key, JSON.stringify(value)); }
  catch { /* The session can continue with in-memory preferences. */ }
}
const savedPins = saved('flock-pins', []);
const pins = new Set(Array.isArray(savedPins) ? savedPins : []);
const drafts = new Map();
const replies = new Map();
const sendingMessages = new Set();
const pendingMessages = new Map();
const pendingWaits = new Set();
const notices = new Map();
const signatures = new Map();
let bots = [];
let listLoaded = false;
let listGeneration = 0;
let view = null;
let menu = null;
let detail = null;
let profileAvatar = null;
let profileKey = '';
let connection = 'loading';

function node(tag, className, text) {
  const el = document.createElement(tag);
  if (className) el.className = className;
  if (text != null) el.textContent = text;
  return el;
}
function button(text, className, click) {
  const el = node('button', className, text);
  el.type = 'button';
  if (click) el.addEventListener('click', click);
  return el;
}
function avatar(bot, className = '') {
  const host = node('span', `avatar ${className}`.trim());
  showAvatar(host, bot);
  return host;
}
function linkToRun(run, label = 'Open in T3') {
  if (!run?.link) return null;
  const url = new URL(run.link);
  const hosted = url.origin === 'https://app.t3.codes' && url.pathname.startsWith('/threads/');
  const local = url.protocol === 'http:' && ['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname);
  if ((!hosted && !local) || url.username || url.password) return null;
  const link = node('a', 'link', `${label} ↗`);
  link.href = url.href;
  link.target = '_blank';
  link.rel = 'noopener noreferrer';
  return link;
}
function waitLabel(reason) { return waitLabels[reason]?.[0] ?? 'Waiting for input'; }
function waitText(wait) { return wait.text || waitLabels[wait.reason]?.[1] || 'Refresh the pending request.'; }
function badge(state) { return node('span', `badge ${state}`, stateLabels[state] ?? state); }
function changed(key, ...values) {
  const signature = JSON.stringify(values);
  if (signatures.get(key) === signature) return false;
  signatures.set(key, signature);
  return true;
}
async function api(path, body) {
  const init = { credentials: 'same-origin' };
  if (body) Object.assign(init, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Flock-Action': '1' },
    body: JSON.stringify(body),
  });
  const response = await fetch(`/api/${path}`, init);
  const data = await response.json();
  if (!response.ok) throw Object.assign(new Error(data.error), { status: response.status });
  return data;
}
const botQuery = (path, bot, before) => `${path}?${new URLSearchParams(before ? { bot, before } : { bot })}`;
const messageStorageKey = (bot) => `flock-pending-message:${bot}`;

function pendingMessage(bot) {
  if (!pendingMessages.has(bot)) {
    try {
      const pending = JSON.parse(sessionStorage.getItem(messageStorageKey(bot)));
      if (pending?.payload?.botId === bot) pendingMessages.set(bot, pending);
    } catch {}
  }
  return pendingMessages.get(bot);
}
function rememberMessage(bot, pending) {
  if (pending) pendingMessages.set(bot, pending);
  else pendingMessages.delete(bot);
  try {
    if (pending) sessionStorage.setItem(messageStorageKey(bot), JSON.stringify(pending));
    else sessionStorage.removeItem(messageStorageKey(bot));
  } catch {}
}

function setConnection(state) {
  if (connection === state) return;
  connection = state;
  elements.connection.dataset.state = state;
  elements['connection-text'].textContent = state === 'offline' ? 'Could not connect to the runtime' : '';
  renderHeader();
}
function renderTheme(mode) {
  for (const option of elements['theme-switch'].querySelectorAll('button')) {
    option.setAttribute('aria-pressed', option.dataset.themeMode === mode);
  }
}
function setTheme(mode) {
  save('flock-theme', mode);
  document.dispatchEvent(new CustomEvent('flock-theme', { detail: mode }));
  renderTheme(mode);
}
function openNavigation(open, focus = true) {
  const expanded = open && narrow.matches;
  workspace.classList.toggle('nav-open', expanded);
  elements['open-navigation'].setAttribute('aria-expanded', expanded);
  if (focus && narrow.matches) elements[expanded ? 'close-navigation' : 'open-navigation'].focus();
}
function selectBot(botId) {
  menu = null;
  openNavigation(false);
  if (view?.id === botId) { renderBots(); return; }
  closeDetail();
  view = botId ? { id: botId, data: null, events: new Map(), olderBefore: 0, loadedSessions: new Map(),
    sessionsBefore: 0, loadingOlder: false, loadingSessions: false, generation: 0 } : null;
  const pending = botId && pendingMessage(botId);
  if (pending?.reply && !replies.has(botId)) replies.set(botId, pending.reply);
  else if (pending && !pending.reply && !drafts.has(botId)) drafts.set(botId, pending.payload.text);
  save('flock-selected-bot', botId);
  render();
  elements.conversation.scrollTop = 0;
  if (view) void refreshView(view);
}

function renderBots() {
  if (!changed('bots', bots, [...pins], menu, view?.id, listLoaded)) return;
  const pinned = [];
  const rest = [];
  for (const bot of bots) {
    const isPinned = pins.has(bot.id);
    const row = node('div', isPinned ? 'pinned-bot' : 'bot-row');
    const active = view?.id === bot.id;
    row.classList.toggle('selected', active);
    const select = button(null, isPinned ? 'pin' : 'bot-select', () => selectBot(bot.id));
    select.title = bot.name;
    select.setAttribute('aria-label', bot.name);
    if (active) select.setAttribute('aria-current', 'true');
    select.append(avatar(bot));
    if (!isPinned) {
      const text = node('span', 'bot-text');
      text.append(node('span', 'bot-name', bot.name), node('span', 'bot-preview', bot.preview || 'No conversation yet'));
      select.append(text);
    }
    const toggle = button('⋯', 'icon-button pin-menu', () => {
      const opened = menu !== bot.id;
      menu = opened ? bot.id : null;
      renderBots();
      const replacement = [...document.querySelectorAll('.pin-menu')].find((el) => el.dataset.bot === bot.id);
      if (opened) replacement?.nextElementSibling?.querySelector('button')?.focus();
      else replacement?.focus();
    });
    toggle.dataset.bot = bot.id;
    toggle.setAttribute('aria-label', `Menu for ${bot.name}`);
    toggle.setAttribute('aria-expanded', menu === bot.id);
    row.append(select, toggle);
    if (menu === bot.id) {
      const popover = node('div', 'pin-popover');
      popover.append(button(isPinned ? 'Unpin' : 'Pin', null, () => {
        if (isPinned) pins.delete(bot.id);
        else pins.add(bot.id);
        save('flock-pins', [...pins]);
        menu = null;
        renderBots();
        [...document.querySelectorAll('.pin-menu')].find((el) => el.dataset.bot === bot.id)?.focus();
      }));
      row.append(popover);
    }
    (isPinned ? pinned : rest).push(row);
  }
  elements.pinned.replaceChildren(...pinned);
  elements.pinned.hidden = !pinned.length;
  elements['bot-list'].replaceChildren(...rest);
  if (!bots.length) elements['bot-list'].append(node('p', 'activity-empty', listLoaded ? 'No Bots configured' : 'Loading…'));
}

function renderHeader() {
  const bot = view?.data?.bot ?? bots.find((item) => item.id === view?.id);
  elements.profile.hidden = !bot;
  elements['settings-button'].hidden = !bot;
  elements['profile-placeholder'].hidden = !!bot;
  if (!bot) {
    profileAvatar?.destroy();
    profileAvatar = null;
    profileKey = '';
    return;
  }
  const presence = view?.data?.presence ?? 'idle';
  const avatarState = presence === 'peer_waiting' ? 'waiting' : presence;
  const key = JSON.stringify([bot.id, bot.avatar]);
  const avatarState = presence === 'stopping' ? 'thinking' : presence === 'queued' ? 'idle' : presence;
  if (key !== profileKey) {
    profileAvatar?.destroy();
    profileAvatar = showAvatar(elements['profile-avatar'], bot, { still: false, state: avatarState });
    profileKey = key;
  } else if (elements['profile-status'].dataset.state !== presence) profileAvatar.update({ state: avatarState });
  elements['profile-name'].textContent = bot.name;
  elements.profile.setAttribute('aria-label', `Open settings for ${bot.name}`);
  elements['profile-status'].dataset.state = presence;
  elements['profile-status'].textContent = connection === 'offline' ? 'Disconnected from the runtime' : stateLabels[presence];
}

function activityTitle(request, className) {
  const title = node('span', className, request);
  title.title = request;
  return title;
}
function renderActivity() {
  const data = view?.data;
  const waits = data?.waits ?? [];
  const clarifications = data?.clarifications ?? [];
  if (changed('waits', view?.id, waits, clarifications)) {
    elements['wait-count'].textContent = waits.length + clarifications.length;
    elements['wait-count'].classList.toggle('has-waits', waits.length + clarifications.length > 0);
    const rows = waits.map((task) => {
      const row = button(null, 'activity wait-select', () => {
        openNavigation(false);
        openDetail({ kind: 'wait', taskId: task.id, waitId: task.wait.id });
      });
      const line = node('span', 'activity-line');
      line.append(node('span', 'wait-dot'), node('span', 'activity-title', waitLabel(task.wait.reason)));
      row.append(line, activityTitle(task.request, 'activity-subtitle'));
      return row;
    });
    for (const clarification of clarifications) {
      const row = button(null, 'activity wait-select', () => {
        openNavigation(false);
        openDetail({ kind: 'clarification', messageId: clarification.messageId });
      });
      const line = node('span', 'activity-line');
      line.append(node('span', 'wait-dot'), node('span', 'activity-title', 'Clarification requested'));
      row.append(line, activityTitle(clarification.text, 'activity-subtitle'));
      rows.push(row);
    }
    elements['wait-list'].replaceChildren(...rows);
    if (!rows.length) elements['wait-list'].append(node('p', 'activity-empty', 'Nothing waiting for input'));
  }
  const groups = new Map(view?.loadedSessions);
  for (const task of data?.sessionTasks ?? []) groups.set(task.id, task);
  const active = data?.activeSessions ?? [];
  const more = elements['more-sessions'];
  more.hidden = !view?.sessionsBefore;
  more.disabled = !!view?.loadingSessions;
  if (!changed('sessions', view?.id, [...groups.values()], active)) return;
  const rows = [];
  for (const task of [...groups.values()].sort((a, b) => b.id - a.id)) {
    for (const session of task.sessions) {
      const row = node('article', 'activity session-entry');
      const status = active.find((run) => run.taskId === task.id && run.threadId === session.threadId &&
        run.environmentId === session.environmentId)?.state ?? 'ended';
      const line = node('span', 'activity-line');
      line.append(badge(status), button('Task details', 'text-button', () => {
        openNavigation(false);
        void openTask(task.id);
      }));
      row.append(activityTitle(task.request, 'activity-title'), line);
      const link = linkToRun(session);
      if (link) row.append(link);
      rows.push(row);
    }
  }
  elements['session-list'].replaceChildren(...rows);
  if (!rows.length) elements['session-list'].append(node('p', 'activity-empty', 'No sessions yet'));
}

function beginReply(target, text = '') {
  if (!target.messageId || sendingMessages.has(view.id)) return;
  const current = replies.get(view.id);
  if (current?.messageId !== target.messageId || text)
    replies.set(view.id, { ...target, text });
  closeDetail();
  renderComposer();
  elements.message.focus();
}
function questionActions(task) {
  const target = { kind: 'wait', messageId: task.wait.messageId, summary: waitText(task.wait) };
  const actions = node('div', 'event-actions');
  for (const choice of task.wait.choices ?? []) {
    const option = button(choice, 'button', () => beginReply(target, choice));
    option.disabled = pendingWaits.has(task.wait.id);
    actions.append(option);
  }
  const reply = button('Write a reply', 'text-button', () => beginReply(target));
  reply.disabled = pendingWaits.has(task.wait.id);
  actions.append(reply);
  return actions;
}
function clarificationActions(clarification) {
  const target = { kind: 'clarification', messageId: clarification.messageId, summary: clarification.text };
  const actions = node('div', 'event-actions');
  if (clarification.destinations) {
    for (const destination of clarification.destinations)
      actions.append(button(destination, 'button', () => beginReply(target, destination)));
  } else {
    for (const candidate of clarification.candidates ?? [])
      actions.append(button(candidate.request, 'button', () => beginReply(target, `#${candidate.taskId}`)));
  }
  actions.append(button('Write a reply', 'text-button', () => beginReply(target)));
  return actions;
}
function detailText(label, value) {
  const row = node('p', 'event-detail');
  row.append(node('strong', null, label), document.createTextNode(value));
  return row;
}
function eventTime(event) {
  const timestamp = new Date(event.time * 1000);
  const time = node('time', null, clock.format(timestamp));
  time.dateTime = timestamp.toISOString();
  return time;
}
function sourceAction(source, label = 'Open source') {
  return button(label, 'text-button', async () => {
    if (!source.messageId && source.taskId) {
      await openTask(source.taskId);
      return;
    }
    const target = byId(`event-${source.messageId}`);
    if (target) {
      closeDetail();
      target.focus();
      target.scrollIntoView({ block: 'center' });
      return;
    }
    const context = view;
    const selection = { kind: 'source' };
    openDetail(selection);
    try {
      const page = await api(botQuery('conversation', context.id, source.messageId + 1));
      if (detail !== selection) return;
      selection.event = page.events.find((event) => event.id === source.messageId);
      selection.failed = !selection.event;
    } catch {
      if (detail !== selection) return;
      selection.failed = true;
    }
    renderDetail();
  });
}
function resultActions(event, text, run) {
  const actions = node('div', 'event-actions');
  const link = linkToRun(run);
  if (link) actions.append(link);
  if (text) actions.append(button('Reply to this result', 'text-button', () =>
    beginReply({ kind: 'result', messageId: event.id, summary: text })));
  return actions;
}
function eventCard(event, waits, closed, queued, clarifications) {
  const data = event.data;
  const outbound = ['conversation_message', 'user_answer'].includes(event.kind) ||
    (event.kind === 'request' && data.source !== 'cron');
  const row = node('article', `event${outbound ? ' outbound' : ''}`);
  row.id = `event-${event.id}`;
  row.tabIndex = -1;
  const card = node('div', 'event-card');
  const metadata = node('div', 'event-meta');
  const labels = { request: data.source === 'cron' ? 'Recurring request' : 'You', user_answer: 'Your answer',
    conversation_message: 'You', conversation_reply: view.data.bot.name,
    report: 'Session report', completed: 'Completion report', cancelled: 'Task cancelled' };
  const label = event.kind === 'waiting' ? waitLabel(data.reason) : labels[event.kind];
  metadata.append(node('strong', null, label), node('span', null, `#${event.taskId}`), eventTime(event));
  if (event.taskId && ['request', 'completed', 'report', 'conversation_message'].includes(event.kind)) {
    metadata.append(button('Task details', 'text-button event-task', () => void openTask(event.taskId)));
  }
  row.append(metadata, card);
  if (event.kind === 'waiting') {
    card.classList.add('question-card');
    card.append(node('p', 'event-text', waitText(data)));
    if (data.decisionReason) card.append(detailText('Reason for decision', data.decisionReason));
    const task = waits.get(data.id);
    if (task?.wait.reason === 'user_question') card.append(questionActions(task));
    else if (task) {
      const actions = node('div', 'event-actions');
      actions.append(button('View pending request', 'button', () => openDetail({ kind: 'wait', taskId: task.id, waitId: data.id })));
      card.append(actions);
    } else card.append(node('p', 'event-note', closedLabels[closed.get(data.id)] ?? 'Request closed'));
  } else if (event.kind === 'report') {
    card.append(node('p', 'event-text', data.report || 'The session submitted a report.'));
    if (data.status) card.append(detailText('Run status', stateLabels[data.status] ?? data.status));
    if (data.truncated) card.append(node('p', 'event-note', 'Open T3 to read the full report.'));
    card.append(resultActions(event, data.report, data.run));
  } else if (event.kind === 'completed') {
    card.classList.add('finished-card');
    card.append(node('p', 'event-text', data.text));
    if (data.reason) card.append(detailText('Reason for decision', data.reason));
    card.append(resultActions(event, data.text, data.run));
  } else if (event.kind === 'conversation_reply') {
    card.append(node('p', 'event-text', data.text));
    const clarification = clarifications.get(data.messageId);
    if (clarification) {
      card.classList.add('question-card');
      card.append(clarificationActions(clarification));
    }
    if (data.source?.messageId) {
      const actions = node('div', 'event-actions');
      actions.append(sourceAction(data.source));
      if (data.route === 'respond') actions.append(button('Reply to this answer', 'text-button', () =>
        beginReply({ kind: 'result', messageId: event.id, summary: data.text })));
      card.append(actions);
    }
  } else if (event.kind === 'cancelled') card.append(node('p', 'event-text', 'The task was cancelled.'));
  else {
    card.append(node('p', 'event-text', data.text));
    if (event.kind === 'conversation_message' && data.replyToMessageId) {
      card.append(sourceAction({ messageId: data.replyToMessageId }, 'Open original message'));
    }
    if (data.status === 'received') card.append(node('p', 'event-note', 'Received'));
    if (data.deliveredToT3) card.append(node('p', 'event-note', 'Sent to T3'));
    if (data.source === 'cron' && data.scheduledAt) card.append(node('p', 'event-note', `Scheduled for ${fullTime.format(data.scheduledAt * 1000)}`));
    const blocker = queued.get(event.taskId)?.blockedBy;
    if (['request', 'conversation_message'].includes(event.kind) && blocker) {
      card.append(node('p', 'event-note', 'Waiting for earlier work on the same project.'));
      if (blocker.request) card.append(node('p', 'event-note', blocker.request));
    }
  }
  return row;
}

function peerConversation(messages, activeBot, names) {
  const first = messages[0];
  const { fromBotId, toBotId } = first.data;
  const name = (id) => names.get(id) ?? id;
  const otherBot = name(fromBotId === activeBot ? toBotId : fromBotId);
  const group = node('details', 'peer-exchange');
  group.dataset.firstMessage = first.id;
  const summary = node('summary');
  summary.append(node('span', null, `${otherBot} conversation (${messages.length} messages)`),
    node('span', 'peer-exchange-task', `#${first.taskId}`));
  const list = node('ol', 'peer-message-list');
  list.setAttribute('role', 'list');
  for (const message of messages) {
    const { fromBotId: from, toBotId: to, messageType, requestKind, text, evidence } = message.data;
    const item = node('li', 'peer-message');
    const metadata = node('div', 'event-meta');
    const label = messageType === 'request' && requestKind === 'consult'
      ? 'Consultation' : peerMessageLabels[messageType] ?? 'Message';
    metadata.append(node('strong', null, `${name(from)} → ${name(to)}`),
      node('span', null, label), eventTime(message));
    item.append(metadata, node('p', 'event-text', text));
    for (const reference of evidence ?? []) {
      const link = linkToRun(reference, 'View evidence in T3');
      if (link) item.append(link);
    }
    list.append(item);
  }
  group.append(summary, list);
  return group;
}

function renderTimeline(scroll = 'auto') {
  const older = elements.older;
  const data = view?.data;
  const events = [...(view?.events.values() ?? [])].sort((a, b) => a.id - b.id);
  older.hidden = !view?.olderBefore;
  older.disabled = !!view?.loadingOlder;
  const names = new Map(bots.map((bot) => [bot.id, bot.name]));
  if (!changed('timeline', view?.id, data?.bot, events, data?.waits, data?.queued, data?.clarifications,
    [...pendingWaits], listLoaded, [...names])) return;
  const scroller = elements.conversation;
  const previousHeight = scroller.scrollHeight;
  const previousTop = scroller.scrollTop;
  const atBottom = previousHeight - previousTop - scroller.clientHeight < 65;
  const waits = new Map((data?.waits ?? []).map((task) => [task.wait.id, task]));
  const clarifications = new Map((data?.clarifications ?? []).map((item) => [item.messageId, item]));
  const queued = new Map((data?.queued ?? []).map((task) => [task.id, task]));
  const closed = new Map(events.filter((event) => event.kind === 'wait_closed').map((event) => [event.data.waitId, event.data.resolution]));
  const opened = new Set([...elements.timeline.querySelectorAll('.peer-exchange[open]')]
    .map((group) => group.dataset.firstMessage));
  const focused = elements.timeline.contains(document.activeElement)
    ? document.activeElement.closest('.peer-exchange > summary')?.parentElement.dataset.firstMessage : null;
  let focusTarget = null;
  const fragment = document.createDocumentFragment();
  let peerMessages = [];
  const appendPeerMessages = () => {
    if (!peerMessages.length) return;
    const group = peerConversation(peerMessages, view.id, names);
    if (peerMessages.some((message) => opened.has(String(message.id)))) group.open = true;
    if (peerMessages.some((message) => String(message.id) === focused)) focusTarget = group.firstElementChild;
    fragment.append(group);
    peerMessages = [];
  };
  let day = '';
  for (const event of events) {
    if (event.kind === 'wait_closed') continue;
    const eventDay = dayFormat.format(event.time * 1000);
    if (eventDay !== day || event.kind !== 'bot_message' || peerMessages[0]?.data.exchangeId !== event.data.exchangeId)
      appendPeerMessages();
    if (eventDay !== day) {
      fragment.append(node('p', 'day', eventDay));
      day = eventDay;
    }
    if (event.kind === 'bot_message') peerMessages.push(event);
    else fragment.append(eventCard(event, waits, closed, queued, clarifications));
  }
  appendPeerMessages();
  if (!fragment.childElementCount) {
    const empty = node('div', 'empty-state');
    if (data?.bot) empty.append(avatar(data.bot));
    const loaded = !!data || (!view && listLoaded);
    empty.append(node('h2', null, loaded ? (data ? `Conversation with ${data.bot.name}` : 'No Bots') : 'Loading…'));
    fragment.append(empty);
  }
  elements.timeline.replaceChildren(fragment);
  focusTarget?.focus({ preventScroll: true });
  if (scroll === 'older') scroller.scrollTop = previousTop + scroller.scrollHeight - previousHeight;
  else if (scroll === 'bottom' || atBottom) scroller.scrollTop = scroller.scrollHeight;
  else scroller.scrollTop = previousTop;
}

function setNotice(context, text, isError = false) {
  notices.set(context.id, { text, isError });
  if (view?.id === context.id) renderComposer();
}
const replyLabels = { wait: 'Reply to a question', clarification: 'Reply to a clarification', result: 'Reply to a result' };
const composerLimit = (reply) => reply?.kind === 'wait' ? 8000 : 16000;
function renderComposer() {
  const context = view;
  elements['composer-container'].hidden = !context?.data;
  if (!context?.data) return;
  const reply = replies.get(context.id);
  const { text, isError } = notices.get(context.id) ?? {};
  const body = reply ? reply.text : drafts.get(context.id) ?? '';
  const busy = sendingMessages.has(context.id);
  elements['reply-context'].hidden = !reply;
  elements['reply-label'].textContent = reply ? replyLabels[reply.kind] : '';
  elements['reply-question'].textContent = reply?.summary ?? '';
  elements['compose-label'].textContent = reply ? replyLabels[reply.kind] : 'Message';
  elements['cancel-reply'].disabled = busy;
  elements.message.placeholder = reply ? 'Write a reply' : `Message ${context.data.bot.name}`;
  if (elements.message.value !== body) elements.message.value = body;
  elements.message.readOnly = busy;
  elements.send.disabled = busy || !body.trim() || byteLength(body) > composerLimit(reply);
  elements.send.setAttribute('aria-label', reply ? 'Send reply' : 'Send message');
  elements.notice.textContent = text ?? '';
  elements.notice.classList.toggle('error', !!isError);
}

function closeDetail() {
  detail = null;
  if (elements.detail.open) elements.detail.close();
}
function openDetail(selection) {
  if (!view?.data) return;
  detail = selection;
  signatures.delete('detail');
  renderDetail();
  if (!elements.detail.open) elements.detail.showModal();
}
async function openTask(taskId) {
  const context = view;
  if (!context?.data) return;
  const selection = { kind: 'task', taskId };
  openDetail(selection);
  try {
    const task = await api(`task?${new URLSearchParams({ bot: context.id, taskId })}`);
    if (detail !== selection) return;
    selection.task = task;
  } catch {
    if (detail !== selection) return;
    selection.failed = true;
  }
  renderDetail();
}
function taskContent(task) {
  const purpose = node('section', 'dialog-section');
  purpose.append(badge(task.state), node('h3', null, 'Goal'), node('p', 'prose', task.intent || task.request));
  const parts = [purpose];
  if (task.revisions.length || task.doneWhen) {
    const revisions = node('section', 'dialog-section');
    revisions.append(node('h3', null, 'Completion criteria'));
    if (!task.revisions.length) revisions.append(node('p', 'prose', task.doneWhen));
    for (const revision of [...task.revisions].reverse()) {
      const row = node('div', 'schedule-row');
      const label = revision.id === task.currentRevision ? `Revision ${revision.number} (current)` : `Revision ${revision.number}`;
      row.append(node('p', 'dialog-meta', label), node('p', 'prose', revision.doneWhen));
      if (revision.intent && revision.intent !== (task.intent || task.request)) row.append(detailText('Goal in this revision', revision.intent));
      if (revision.sourceMessageId) row.append(sourceAction({ messageId: revision.sourceMessageId }, 'Open change request'));
      revisions.append(row);
    }
    parts.push(revisions);
  }
  if (task.pendingInstructions.length) {
    const pending = node('section', 'dialog-section');
    pending.append(node('h3', null, 'Instructions pending delivery'));
    for (const instruction of task.pendingInstructions) pending.append(node('p', 'prose', instruction.text));
    parts.push(pending);
  }
  if (task.result) {
    const result = node('section', 'dialog-section');
    const data = task.result.data;
    const label = task.result.kind === 'completed' ? 'Completion report' : 'Latest session report';
    result.append(node('h3', null, label), node('p', 'prose', data.text ?? data.report));
    const actions = node('div', 'event-actions');
    actions.append(sourceAction({ messageId: task.result.id }, 'Open report in conversation'));
    const link = linkToRun(data.run);
    if (link) actions.append(link);
    result.append(actions);
    parts.push(result);
  }
  if (task.sourceResultRefs.length || task.sourceTaskId) {
    const sources = node('section', 'dialog-section');
    sources.append(node('h3', null, 'Original task'));
    for (const source of task.sourceResultRefs) {
      if (source.text) sources.append(node('p', 'prose', source.text));
      sources.append(sourceAction(source, source.messageId ? 'Open original result' : 'Open original task'));
    }
    if (!task.sourceResultRefs.length)
      sources.append(button('Open original task', 'text-button', () => void openTask(task.sourceTaskId)));
    parts.push(sources);
  }
  if (task.runs.length) {
    const runs = node('section', 'dialog-section');
    runs.append(node('h3', null, 'Run history'));
    for (const run of [...task.runs].reverse()) {
      const row = node('div', 'schedule-row');
      const revision = task.revisions.find((item) => item.id === run.revisionId);
      const label = [run.runId, run.environmentId, revision ? `Revision ${revision.number}` : ''].filter(Boolean).join(' · ');
      row.append(node('p', 'dialog-meta', label));
      const link = linkToRun(run);
      if (link) row.append(link);
      runs.append(row);
    }
    parts.push(runs);
  }
  return parts;
}
function settingsContent(bot) {
  elements['detail-title'].textContent = 'Bot settings';
  const profile = node('div', 'dialog-profile');
  profile.append(avatar(bot), node('span', null, bot.name));
  const role = node('section', 'dialog-section');
  role.append(node('h3', null, 'Role'), node('p', 'prose', bot.soul || 'No role configured.'));
  const schedule = node('section', 'dialog-section');
  schedule.append(node('h3', null, 'Recurring requests'));
  for (const item of bot.cron) {
    const row = node('div', 'schedule-row');
    const specification = [item.schedule ?? item.expression ?? item.cron, item.timezone ?? item.timeZone, item.id,
      item.enabled === false ? 'Disabled' : ''].filter(Boolean).join(' · ');
    row.append(node('p', 'prose', item.request), node('p', 'dialog-meta', specification));
    schedule.append(row);
  }
  if (!bot.cron.length) schedule.append(node('p', 'prose', 'No recurring requests.'));
  return [profile, role, schedule];
}
function waitForm(label, fields, submit) {
  const form = node('form', 'wait-form');
  for (const field of fields) {
    const wrapper = node('label', null, field.label);
    const input = node('input');
    Object.assign(input, { name: field.name, type: field.type ?? 'text', value: field.value ?? '', required: field.required ?? false });
    wrapper.append(input);
    form.append(wrapper);
  }
  const action = button(label, 'button');
  action.type = 'submit';
  form.append(action);
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    submit(Object.fromEntries(new FormData(form)));
  });
  return form;
}
function localDateTime(timestamp) {
  const date = new Date(timestamp);
  return new Date(timestamp - date.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
}
function waitContent(context, task) {
  const wait = task.wait;
  elements['detail-title'].textContent = waitLabel(wait.reason);
  const request = node('section', 'dialog-section');
  request.append(node('h3', null, 'Task'), node('p', 'dialog-request', task.request));
  const reason = node('section', 'dialog-section');
  reason.append(node('h3', null, 'Reason for waiting'), node('p', 'prose', waitText(wait)));
  if (wait.decisionReason) reason.append(detailText('Reason for decision', wait.decisionReason));
  const actions = node('section', 'dialog-section');
  actions.append(node('h3', null, 'Available actions'));
  const buttons = node('div', 'event-actions');
  const runLink = linkToRun(wait.run, wait.reason === 't3_input' ? 'Approve in T3' : 'Open in T3');
  if (runLink) buttons.append(runLink);
  const perform = (operation, fields) => void operate(context, task, operation, fields);
  if (wait.reason === 'user_question') actions.append(questionActions(task));
  else if (wait.reason === 'budget') buttons.append(button('Approve one more run', 'button', () => perform('approve')));
  else if (wait.reason === 'model_failure') buttons.append(button('Retry decision', 'button', () => perform('retry')));
  else if (wait.reason === 'deadline') {
    if (task.deadline) reason.append(node('p', 'dialog-meta', `Current deadline · ${fullTime.format(task.deadline * 1000)}`));
    actions.append(waitForm('Extend deadline', [{ name: 'deadline', label: 'New deadline (local time)',
      type: 'datetime-local', value: localDateTime(Date.now() + 60 * 60 * 1000), required: true }], ({ deadline }) => {
      const next = Math.floor(new Date(deadline).getTime() / 1000);
      if (next > Date.now() / 1000) perform('extend', { deadline: next });
      else showDetailError(errors.invalid_deadline);
    }));
  } else if (['unknown_delivery', 'auth', 'settings'].includes(wait.reason)) {
    buttons.append(button('Reconcile', 'button', () => perform('reconcile')));
    if (wait.reason === 'unknown_delivery') {
      const advanced = node('details');
      advanced.append(node('summary', 'dialog-meta', 'Specify the assigned run'), waitForm('Reconcile with this run', [
        { name: 'threadId', label: 'Thread ID', required: true }, { name: 'runId', label: 'Run ID', required: true },
        { name: 'questionId', label: 'Question ID (optional)' },
      ], (fields) => perform('reconcile', Object.fromEntries(Object.entries(fields).filter(([, value]) => value.trim())))));
      actions.append(advanced);
    }
  }
  buttons.append(button('Cancel task', 'button danger', () => {
    if (window.confirm(`Cancel “${task.request}”?`)) perform('cancel');
  }));
  actions.append(buttons);
  const feedback = node('p', 'dialog-feedback');
  feedback.id = 'detail-feedback';
  feedback.setAttribute('role', 'status');
  return [request, reason, actions, feedback];
}
function showDetailError(text) {
  const output = byId('detail-feedback');
  if (output) output.textContent = text;
}
function renderDetail() {
  if (!detail || !view?.data) return;
  if (detail.kind === 'settings') {
    const bot = view.data.bot;
    if (changed('detail', detail, bot)) elements['detail-body'].replaceChildren(...settingsContent(bot));
    return;
  }
  if (detail.kind === 'task') {
    if (changed('detail', detail)) {
      elements['detail-title'].textContent = 'Task details';
      elements['detail-body'].replaceChildren(...(detail.task ? taskContent(detail.task) :
        [node('p', 'prose', detail.failed ? 'Could not load task details.' : 'Loading…')]));
    }
    return;
  }
  if (detail.kind === 'source') {
    if (changed('detail', detail)) {
      elements['detail-title'].textContent = 'Source';
      const source = detail.event;
      const text = source?.kind === 'waiting' ? waitText(source.data) :
        source?.data.report ?? source?.data.text ?? (detail.failed ? 'Could not load the source.' : 'Loading…');
      const body = node('p', 'prose', text);
      elements['detail-body'].replaceChildren(body);
      if (detail.event?.taskId) elements['detail-body'].append(
        button('Task details', 'text-button', () => void openTask(detail.event.taskId)));
    }
    return;
  }
  if (detail.kind === 'clarification') {
    const clarification = view.data.clarifications.find((item) => item.messageId === detail.messageId);
    if (changed('detail', detail, clarification)) {
      elements['detail-title'].textContent = 'Clarification requested';
      elements['detail-body'].replaceChildren(node('p', 'prose', clarification?.text ?? 'This request has closed.'));
      if (clarification) elements['detail-body'].append(clarificationActions(clarification));
    }
    return;
  }
  const task = view.data.waits.find((item) => item.id === detail.taskId && item.wait.id === detail.waitId);
  if (!task) {
    if (changed('detail', detail, null)) {
      elements['detail-title'].textContent = 'Request closed';
      elements['detail-body'].replaceChildren(node('p', 'prose', 'Check the conversation for the latest update.'));
    }
    return;
  }
  if (changed('detail', detail, task)) elements['detail-body'].replaceChildren(...waitContent(view, task));
  for (const control of elements['detail-body'].querySelectorAll('button, input')) control.disabled = pendingWaits.has(task.wait.id);
}
function render(scroll) {
  renderBots();
  renderHeader();
  renderActivity();
  renderTimeline(scroll);
  renderComposer();
  renderDetail();
}

function mergeLatest(context, latest) {
  const { events, olderBefore } = latest;
  const first = events[0]?.id;
  // A long disconnection can leave a gap larger than the newest page.
  // Re-anchor pagination at the newest page so the next older page fills it.
  if (!context.events.size || (first && first > context.newestEvent)) {
    context.events.clear();
    context.olderBefore = olderBefore;
  }
  for (const event of events) context.events.set(event.id, event);
  context.newestEvent = events.at(-1)?.id ?? context.newestEvent;
  if (!context.loadedSessions.size) context.sessionsBefore = latest.sessionsBefore;
  context.data = latest;
  const reply = replies.get(context.id);
  if (reply?.kind === 'wait' && !latest.waits.some((task) => task.wait.messageId === reply.messageId)) {
    replies.delete(context.id);
    setNotice(context, 'That question is no longer open.');
  }
}
async function refreshView(context, scroll) {
  const generation = ++context.generation;
  try {
    const latest = await api(botQuery('view', context.id));
    if (context !== view || generation !== context.generation) return;
    mergeLatest(context, latest);
    setConnection('ready');
    render(scroll);
  } catch (error) {
    if (context !== view || generation !== context.generation) return;
    if (error.status === 404) void refreshBots();
    else setConnection('offline');
  }
}
async function refreshBots() {
  const generation = ++listGeneration;
  try {
    const latest = await api('bots');
    if (generation !== listGeneration) return;
    bots = latest;
    listLoaded = true;
    setConnection('ready');
    if (!bots.some((bot) => bot.id === view?.id)) {
      const preferred = saved('flock-selected-bot', null);
      selectBot(bots.find((bot) => bot.id === preferred)?.id ?? bots[0]?.id ?? null);
    }
    render();
  } catch {
    if (generation === listGeneration) setConnection('offline');
  }
}
async function loadOlder() {
  const context = view;
  const cursor = context?.olderBefore;
  if (!cursor || context.loadingOlder) return;
  context.loadingOlder = true;
  renderTimeline();
  try {
    const page = await api(botQuery('conversation', context.id, cursor));
    if (context !== view || context.olderBefore !== cursor) return;
    context.olderBefore = page.olderBefore;
    for (const event of page.events) context.events.set(event.id, event);
    renderTimeline('older');
  } catch { if (context === view) setNotice(context, 'Could not load earlier messages.', true); }
  finally {
    context.loadingOlder = false;
    if (context === view) renderTimeline();
  }
}
async function loadSessions() {
  const context = view;
  const cursor = context?.sessionsBefore;
  if (!cursor || context.loadingSessions) return;
  context.loadingSessions = true;
  renderActivity();
  try {
    const page = await api(botQuery('sessions', context.id, cursor));
    if (context !== view) return;
    for (const task of page.sessionTasks) context.loadedSessions.set(task.id, task);
    context.sessionsBefore = page.sessionsBefore;
  } catch { if (context === view) setNotice(context, 'Could not load earlier sessions.', true); }
  finally {
    context.loadingSessions = false;
    if (context === view) renderActivity();
  }
}

async function operate(context, task, operation, fields = {}) {
  const waitId = task.wait.id;
  if (!context || pendingWaits.has(waitId)) return;
  pendingWaits.add(waitId);
  setNotice(context, 'Submitting action…');
  if (view?.id === context.id) render();
  try {
    await api('operations', { botId: context.id, taskId: task.id, waitId, operation, ...fields });
    setNotice(context, operation === 'answer' ? 'Answer received.' : 'Action received.');
    if (view?.id === context.id && detail?.waitId === waitId) closeDetail();
  } catch (error) {
    const message = errors[error.message] ?? 'Flock could not confirm the action. Check the conversation.';
    setNotice(context, message, true);
    if (view?.id === context.id && detail?.waitId === waitId) showDetailError(message);
  } finally {
    if (view?.id === context.id) await refreshView(view);
    pendingWaits.delete(waitId);
    if (view?.id === context.id) render();
  }
}
async function submit(event) {
  event.preventDefault();
  const context = view;
  if (!context?.data || elements.send.disabled) return;
  const reply = replies.get(context.id);
  const text = reply ? reply.text : drafts.get(context.id);
  const replyToMessageId = reply?.messageId;
  const previous = pendingMessage(context.id)?.payload;
  const clientMessageId = previous?.text === text && previous.replyToMessageId === replyToMessageId
    ? previous.clientMessageId : crypto.randomUUID();
  const payload = { botId: context.id, clientMessageId, text };
  if (replyToMessageId) payload.replyToMessageId = replyToMessageId;
  rememberMessage(context.id, { payload, reply });
  sendingMessages.add(context.id);
  setNotice(context, 'Sending message…');
  try {
    await api('messages', payload);
    rememberMessage(context.id, null);
    if (reply) replies.delete(context.id);
    else drafts.delete(context.id);
    setNotice(context, 'Message received.');
    if (view?.id === context.id) await refreshView(view, 'bottom');
    void refreshBots();
  } catch (error) {
    if (error.status >= 400 && error.status < 500) rememberMessage(context.id, null);
    setNotice(context, errors[error.message] ?? 'Flock could not confirm receipt. You can resend the same message.', true);
  }
  finally {
    sendingMessages.delete(context.id);
    if (view?.id === context.id) renderComposer();
  }
}

const savedTheme = saved('flock-theme', 'system');
renderTheme(['light', 'dark'].includes(savedTheme) ? savedTheme : 'system');
elements['theme-switch'].addEventListener('click', (event) => {
  const option = event.target.closest('button[data-theme-mode]');
  if (option) setTheme(option.dataset.themeMode);
});
elements['open-navigation'].addEventListener('click', () => openNavigation(true));
elements['close-navigation'].addEventListener('click', () => openNavigation(false));
narrow.addEventListener('change', () => openNavigation(false, false));
for (const id of ['profile', 'settings-button']) elements[id].addEventListener('click', () => openDetail({ kind: 'settings' }));
elements['close-detail'].addEventListener('click', closeDetail);
elements.detail.addEventListener('close', () => { if (!elements.detail.open) detail = null; });
elements.older.addEventListener('click', loadOlder);
elements['more-sessions'].addEventListener('click', loadSessions);
elements.composer.addEventListener('submit', submit);
elements['cancel-reply'].addEventListener('click', () => {
  replies.delete(view.id);
  renderComposer();
  elements.message.focus();
});
elements.message.addEventListener('input', () => {
  if (!view) return;
  const reply = replies.get(view.id);
  if (reply) reply.text = elements.message.value;
  else drafts.set(view.id, elements.message.value);
  notices.delete(view.id);
  const limit = composerLimit(reply);
  if (byteLength(elements.message.value) > limit) setNotice(view, `Messages can be up to ${limit.toLocaleString('en')} bytes.`, true);
  else renderComposer();
});
document.addEventListener('click', (event) => {
  if (menu && !event.target.closest('.pin-menu, .pin-popover')) { menu = null; renderBots(); }
});
document.addEventListener('keydown', (event) => {
  if (event.key !== 'Escape') return;
  if (menu) {
    const bot = menu;
    menu = null;
    renderBots();
    [...document.querySelectorAll('.pin-menu')].find((el) => el.dataset.bot === bot)?.focus();
  } else if (workspace.classList.contains('nav-open') && !elements.detail.open) openNavigation(false);
});

function refresh() {
  if (document.hidden) return;
  void refreshBots();
  if (view) void refreshView(view);
}
render();
refresh();
setInterval(refresh, 2500);
document.addEventListener('visibilitychange', refresh);
window.addEventListener('pageshow', (event) => { if (event.persisted) refresh(); });
