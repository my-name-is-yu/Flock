import { accentColor, showAvatar } from '/avatars.js';

const byId = (id) => document.getElementById(id);
const elements = Object.fromEntries([
  'navigation', 'pinned', 'bot-list', 'wait-list', 'wait-count', 'session-list', 'more-sessions',
  'connection', 'connection-text', 'open-navigation', 'close-navigation', 'profile', 'profile-avatar',
  'profile-name', 'profile-status', 'profile-placeholder', 'settings-button', 'conversation', 'older',
  'timeline', 'composer-container', 'composer', 'reply-context', 'reply-question', 'cancel-reply',
  'reply-label', 'compose-label', 'message', 'send', 'notice', 'detail', 'detail-title', 'detail-body', 'close-detail',
  'theme-switch',
  'peer-heading', 'peer-back', 'peer-picker', 'peer-read-only',
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
  failed: 'Failed', interrupted: 'Interrupted', rolled_back: 'Rolled back',
  idle: 'Idle', thinking: 'Thinking', working: 'Working', peer_waiting: 'Waiting for another Bot',
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
  invalid_peer: 'Choose another Bot for this DM.',
  peer_not_allowed: 'Messaging this Bot from this page is restricted.',
  peer_not_found: 'This Bot DM is unavailable.',
};

function saved(key, fallback) {
  try { return JSON.parse(localStorage.getItem(key)) ?? fallback; }
  catch { return fallback; }
}
function save(key, value) {
  try { localStorage.setItem(key, JSON.stringify(value)); }
  catch {}
}
const savedPins = saved('flock-pins', []);
const pins = new Set(Array.isArray(savedPins) ? savedPins : []);
const drafts = new Map();
const replies = new Map();
const sendingMessages = new Set();
const pendingMessages = new Map();
const pendingWaits = new Map();
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
function runHref(run) {
  if (!run?.link) return null;
  let url;
  try { url = new URL(run.link); }
  catch { return null; }
  const hosted = url.origin === 'https://app.t3.codes' && url.pathname.startsWith('/threads/');
  const local = url.protocol === 'http:' && ['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname);
  if ((!hosted && !local) || url.username || url.password) return null;
  return url.href;
}
function linkToRun(run, label = 'Open in T3') {
  const href = runHref(run);
  if (!href) return null;
  const link = node('a', 'link', `${label} ↗`);
  link.href = href;
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
const peerQuery = ({ id: bot, peerId: peer }, before) =>
  `peer-conversation?${new URLSearchParams(before ? { bot, peer, before } : { bot, peer })}`;
const composerContext = () => view?.dm ?? view;
const composerKey = ({ id, peerId }) => peerId ? JSON.stringify([id, peerId]) : id;
const showingComposer = (key) => view && composerKey(composerContext()) === key;
const messageStorageKey = (key) => `flock-pending-message:${key}`;

function pendingMessage(context) {
  const key = composerKey(context);
  if (!pendingMessages.has(key)) {
    try {
      const pending = JSON.parse(sessionStorage.getItem(messageStorageKey(key)));
      if (pending?.payload?.botId === context.id && pending.payload.peerId === context.peerId)
        pendingMessages.set(key, pending);
    } catch {}
  }
  return pendingMessages.get(key);
}
function rememberMessage(context, pending) {
  const key = composerKey(context);
  if (pending) pendingMessages.set(key, pending);
  else pendingMessages.delete(key);
  try {
    if (pending) sessionStorage.setItem(messageStorageKey(key), JSON.stringify(pending));
    else sessionStorage.removeItem(messageStorageKey(key));
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
  const navigationVisibilityChanged = workspace.classList.contains('nav-open') !== expanded;
  workspace.classList.toggle('nav-open', expanded);
  elements['open-navigation'].setAttribute('aria-expanded', expanded);
  if (focus && narrow.matches && navigationVisibilityChanged) elements[expanded ? 'close-navigation' : 'open-navigation'].focus();
}
function conversationUrl(botId, peerId) {
  const url = new URL(location.href);
  if (botId) url.searchParams.set('bot', botId);
  else url.searchParams.delete('bot');
  if (peerId) url.searchParams.set('peer', peerId);
  else url.searchParams.delete('peer');
  return url;
}
function selectBot(botId, peerId = null, updateHistory = true) {
  menu = null;
  openNavigation(false);
  if (view?.id === botId && (view?.dm?.peerId ?? null) === peerId) { renderBots(); return; }
  closeDetail();
  const existing = view?.id === botId ? view : null;
  view = botId ? { id: botId, data: null, events: new Map(), olderBefore: 0, loadedSessions: new Map(),
    sessionsBefore: 0, ...existing, loadingOlder: false, loadingSessions: false, generation: 0,
    dm: peerId ? { id: botId, peerId, events: new Map(), olderBefore: 0, generation: 0 } : null } : null;
  if (view) {
    const context = composerContext();
    const key = composerKey(context);
    const pending = pendingMessage(context);
    if (pending?.reply && !replies.has(key)) replies.set(key, pending.reply);
    else if (pending && !pending.reply && !drafts.has(key)) drafts.set(key, pending.payload.text);
  }
  const url = conversationUrl(botId, peerId);
  if (url.href !== location.href) {
    if (updateHistory) history.pushState(null, '', url);
    else history.replaceState(null, '', url);
  }
  save('flock-selected-bot', botId);
  elements.conversation.scrollTop = 0;
  render('bottom');
  if (view) void refreshView(view, 'bottom');
}
function selectLocation() {
  const params = new URLSearchParams(location.search);
  const requested = params.get('bot');
  const bot = bots.find((entry) => entry.id === requested);
  const peer = params.get('peer') || null;
  const preferred = saved('flock-selected-bot', null);
  selectBot(bot?.id ?? bots.find((entry) => entry.id === preferred)?.id ?? bots[0]?.id ?? null,
    bot && peer !== bot.id ? peer : null, false);
}
function peerLink(botId, peerId, label, className) {
  const link = node('a', className, label);
  link.href = conversationUrl(botId, peerId);
  link.addEventListener('click', (event) => {
    if (event.button || event.ctrlKey || event.metaKey || event.altKey || event.shiftKey) return;
    event.preventDefault();
    selectBot(botId, peerId);
    elements['peer-heading'].focus({ preventScroll: true });
  });
  return link;
}

const sidebarIcons = {
  more: '<svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><circle cx="6" cy="12" r="1.6"/><circle cx="12" cy="12" r="1.6"/><circle cx="18" cy="12" r="1.6"/></svg>',
  pin: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 4h6l-1 5 3 3v2H7v-2l3-3-1-5Z"/><path d="M12 14v6"/></svg>',
  settings: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 7h3m4 0h9M4 12h9m4 0h3M4 17h3m4 0h9"/><circle cx="9" cy="7" r="2"/><circle cx="15" cy="12" r="2"/><circle cx="9" cy="17" r="2"/></svg>',
  open: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M8 16 16 8M10 8h6v6"/></svg>',
};
const activePresence = new Set(['working', 'thinking', 'waiting']);
const busyPresence = new Set(['working', 'thinking']);
const sidebar = byId('navigation');

function pinnedBotsInShortcutOrder() {
  return [...pins].map((id) => bots.find((bot) => bot.id === id)).filter(Boolean);
}
function focusKey(key) {
  sidebar.querySelector(`[data-focus="${CSS.escape(key)}"]`)?.focus();
}
function setMenu(botId, focus) {
  menu = botId;
  renderBots();
  if (focus === 'first' || focus === 'last') {
    const items = sidebar.querySelectorAll('.menu-popover [role="menuitem"]');
    items[focus === 'first' ? 0 : items.length - 1]?.focus();
  } else if (focus) focusKey(focus);
}
function closeMenu(restore = true) {
  const botId = menu;
  if (!botId) return;
  setMenu(null, restore ? `menu:${botId}` : null);
}
function togglePin(bot) {
  if (pins.has(bot.id)) pins.delete(bot.id);
  else pins.add(bot.id);
  save('flock-pins', [...pins]);
  setMenu(null, `menu:${bot.id}`);
}
let settingsReturnFocus = null;
function openSettingsFromMenu(botId) {
  settingsReturnFocus = botId;
  openDetail({ kind: 'settings' });
}
function openBotSettings(bot) {
  menu = null;
  if (view?.id !== bot.id) selectBot(bot.id);
  else renderBots();
  if (view?.data) openSettingsFromMenu(bot.id);
  else if (view) view.openSettingsOnLoad = bot.id;
}
function menuButton(bot) {
  const toggle = button(null, 'row-menu', () => (menu === bot.id ? closeMenu() : setMenu(bot.id, 'first')));
  toggle.innerHTML = sidebarIcons.more;
  toggle.dataset.focus = `menu:${bot.id}`;
  toggle.setAttribute('aria-label', `${bot.name} menu`);
  toggle.setAttribute('aria-haspopup', 'menu');
  toggle.setAttribute('aria-expanded', menu === bot.id);
  toggle.addEventListener('keydown', (event) => {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    event.preventDefault();
    setMenu(bot.id, event.key === 'ArrowDown' ? 'first' : 'last');
  });
  if (menu === bot.id) toggle.setAttribute('aria-controls', 'bot-menu');
  return toggle;
}
function menuPopover(bot) {
  const list = node('div', 'menu-popover');
  list.id = 'bot-menu';
  list.setAttribute('role', 'menu');
  list.setAttribute('aria-label', `${bot.name} menu`);
  const actions = [
    [pins.has(bot.id) ? 'Unpin' : 'Pin', sidebarIcons.pin, () => togglePin(bot)],
    ['Open settings', sidebarIcons.settings, () => openBotSettings(bot)],
  ];
  actions.forEach(([label, icon, run], index) => {
    const item = button(null, null, run);
    item.innerHTML = icon;
    item.append(node('span', null, label));
    item.setAttribute('role', 'menuitem');
    item.tabIndex = -1;
    item.dataset.focus = `item:${bot.id}:${index}`;
    list.append(item);
  });
  list.addEventListener('keydown', (event) => {
    const items = [...list.querySelectorAll('[role="menuitem"]')];
    const index = items.indexOf(document.activeElement);
    const moves = { ArrowDown: index + 1, ArrowUp: index - 1, Home: 0, End: items.length - 1 };
    if (event.key in moves) {
      event.preventDefault();
      items[(moves[event.key] + items.length) % items.length].focus();
    } else if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      closeMenu();
    } else if (event.key === 'Tab') closeMenu();
  });
  return list;
}
function presenceText(bot) {
  return activePresence.has(bot.presence) ? node('span', 'sr-only', stateLabels[bot.presence]) : null;
}
function botRow(bot) {
  const row = node('div', 'bot-row');
  const active = view?.id === bot.id;
  row.classList.toggle('selected', active);
  row.classList.toggle('menu-open', menu === bot.id);
  const select = button(null, 'bot-select', () => selectBot(bot.id));
  select.dataset.focus = `bot:${bot.id}`;
  if (active) select.setAttribute('aria-current', 'true');
  const name = node('span', 'bot-name');
  name.append(node('span', 'bot-name-text', bot.name));
  if (bot.presence === 'waiting') name.append(node('span', 'presence-dot'));
  const preview = node('span', 'bot-preview');
  if (busyPresence.has(bot.presence)) {
    const dots = node('span', 'working-dots');
    dots.setAttribute('aria-hidden', 'true');
    dots.append(node('i'), node('i'), node('i'));
    preview.append(dots);
  }
  preview.append(bot.preview || 'No conversation yet');
  const text = node('span', 'bot-text');
  text.append(name, preview);
  select.append(avatar(bot, 'bot-avatar'), text);
  const status = presenceText(bot);
  if (status) select.append(status);
  row.append(select, menuButton(bot));
  if (menu === bot.id) row.append(menuPopover(bot));
  return row;
}
function pinTile(bot, index) {
  const tile = node('div', 'pin-tile');
  const active = view?.id === bot.id;
  tile.classList.toggle('selected', active);
  tile.classList.toggle('menu-open', menu === bot.id);
  const select = button(null, 'pin', () => selectBot(bot.id));
  select.dataset.focus = `bot:${bot.id}`;
  if (active) select.setAttribute('aria-current', 'true');
  const shortcut = index < 9 ? index + 1 : null;
  if (shortcut) select.setAttribute('aria-keyshortcuts', `Alt+Shift+${shortcut}`);
  const face = node('span', 'pin-face');
  face.append(avatar(bot, 'pin-avatar'));
  if (activePresence.has(bot.presence)) face.append(node('span', `pin-presence ${bot.presence}`));
  select.append(face, node('span', 'pin-name', bot.name));
  const status = presenceText(bot);
  if (status) select.append(status);
  const tooltip = node('span', 'pin-tooltip');
  tooltip.setAttribute('aria-hidden', 'true');
  tooltip.append(node('span', null, bot.name));
  if (shortcut) tooltip.append(node('kbd', null, `⌥⇧${shortcut}`));
  tile.append(select, tooltip, menuButton(bot));
  if (menu === bot.id) tile.append(menuPopover(bot));
  return tile;
}
function skeletonRows(count, className = 'skeleton-row') {
  return Array.from({ length: count }, () => {
    const row = node('div', className);
    row.setAttribute('aria-hidden', 'true');
    if (className === 'skeleton-bot') row.append(node('span', 'skeleton-avatar'));
    const lines = node('span', 'skeleton-lines');
    lines.append(node('span', 'skeleton-line'), node('span', 'skeleton-line short'));
    row.append(lines);
    return row;
  });
}

function renderBots() {
  if (!changed('bots', bots, [...pins], menu, view?.id, listLoaded)) return;
  const focusedSidebarKey = sidebar.contains(document.activeElement) ? document.activeElement.dataset.focus : null;
  const pinned = pinnedBotsInShortcutOrder();
  const presenceRank = (bot) => (activePresence.has(bot.presence) ? 0 : 1);
  const unpinnedByPresence = bots.filter((bot) => !pins.has(bot.id)).sort((a, b) => presenceRank(a) - presenceRank(b));
  elements.pinned.replaceChildren(...pinned.map(pinTile));
  elements.pinned.hidden = !pinned.length;
  const list = elements['bot-list'];
  list.setAttribute('aria-busy', !listLoaded);
  list.replaceChildren(...unpinnedByPresence.map(botRow));
  if (!listLoaded) list.append(...skeletonRows(4, 'skeleton-bot'));
  else if (!bots.length) list.append(node('p', 'activity-empty', 'No Bots configured'));
  if (focusedSidebarKey) focusKey(focusedSidebarKey);
}

const presenceLabels = { idle: 'Idle', thinking: 'Thinking', working: 'Working', waiting: 'Waiting for input', peer_waiting: 'Waiting for another Bot', stopping: 'Confirming stop', queued: 'Queued' };
const participant = (id) => bots.find((bot) => bot.id === id) ?? { id, name: id };
const botPeers = (id) => participant(id).peers ?? [];

function renderHeader() {
  const bot = view?.data?.bot ?? bots.find((item) => item.id === view?.id);
  const dm = view?.dm;
  const heading = elements['peer-heading'];
  const profile = elements.profile;
  profile.hidden = !bot || !!dm;
  elements['settings-button'].hidden = !bot || !!dm;
  elements['profile-placeholder'].hidden = !!bot || !!dm;
  elements['peer-picker'].hidden = !view?.data || !!dm || !botPeers(bot.id).length;
  elements['peer-back'].hidden = !dm;
  elements['peer-back'].setAttribute('aria-label', bot ? `Back to ${bot.name}` : 'Back to Bot');
  heading.hidden = !dm;
  if (dm) {
    const pair = [dm.id, dm.peerId].sort().map(participant);
    const label = pair.map(({ name }) => name).join(' & ');
    if (changed('peer-heading', pair.map(({ id, name, avatar }) => [id, name, avatar]))) {
      const faces = node('span', 'peer-faces');
      faces.setAttribute('aria-hidden', 'true');
      faces.append(...pair.map((bot) => avatar(bot, 'peer-face')));
      heading.replaceChildren(faces, node('span', 'profile-pill peer-title', label));
      heading.title = label;
    }
  }
  if (!bot || dm) {
    profileAvatar?.destroy();
    profileAvatar = null;
    profileKey = '';
    elements.conversation.style.removeProperty('--bot-accent');
    return;
  }
  const presence = view?.data?.presence ?? 'idle';
  const avatarState = presence === 'peer_waiting' ? 'waiting' :
    presence === 'stopping' ? 'thinking' : presence === 'queued' ? 'idle' : presence;
  const key = JSON.stringify([bot.id, bot.avatar]);
  if (key !== profileKey) {
    profileAvatar?.destroy();
    profileAvatar = showAvatar(elements['profile-avatar'], bot, { still: false, state: avatarState });
    profileKey = key;
    elements.conversation.style.setProperty('--bot-accent', accentColor(bot));
  } else if (profile.dataset.presence !== presence) profileAvatar?.update({ state: presence });
  const offline = connection === 'offline';
  const status = offline ? 'Runtime disconnected' : presenceLabels[presence] ?? presence;
  profile.dataset.presence = presence;
  profile.dataset.state = offline ? 'offline' : presence;
  profile.title = `${bot.name} · ${status}`;
  profile.setAttribute('aria-label', `${bot.name} settings`);
  elements['profile-name'].textContent = bot.name;
  elements['profile-status'].textContent = status;
}

function activityRow(tag, className, title, status, statusClass) {
  const row = node(tag, `activity ${className}`);
  const heading = node('span', 'activity-title', title);
  const line = node('span', `activity-status ${statusClass}`.trim());
  line.append(node('span', 'activity-mark'), node('span', null, status));
  row.append(heading, line);
  return row;
}
function sessionRow(task, session, status, live) {
  const link = runHref(session);
  const row = activityRow(link ? 'a' : 'div', 'session-row', task.request, status, live ? 'live' : '');
  row.title = `#${task.id} ${task.request}`;
  if (link) {
    row.href = link;
    row.target = '_blank';
    row.rel = 'noopener noreferrer';
    row.insertAdjacentHTML('beforeend', sidebarIcons.open);
    row.lastElementChild.classList.add('activity-open');
    row.append(node('span', 'sr-only', ' (Open in T3)'));
  }
  return row;
}
function renderActivity() {
  const data = view?.data;
  const loading = !!view && !data;
  const waits = data?.waits ?? [];
  const clarifications = data?.clarifications ?? [];
  if (changed('waits', view?.id, loading, waits, clarifications)) {
    elements['wait-count'].textContent = waits.length + clarifications.length;
    elements['wait-count'].classList.toggle('has-waits', waits.length + clarifications.length > 0);
    elements['wait-count'].hidden = !(waits.length + clarifications.length);
    elements['wait-list'].setAttribute('aria-busy', loading);
    const rows = waits.map((task) => {
      const row = activityRow('button', 'wait-row', task.request, waitLabel(task.wait.reason), 'waiting');
      row.type = 'button';
      row.title = waitText(task.wait);
      row.addEventListener('click', () => {
        openNavigation(false);
        openDetail({ kind: 'wait', taskId: task.id, waitId: task.wait.id });
      });
      return row;
    });
    for (const clarification of clarifications) {
      const row = button(null, 'activity wait-select', () => {
        openNavigation(false);
        openDetail({ kind: 'clarification', messageId: clarification.messageId });
      });
      const line = node('span', 'activity-line');
      line.append(node('span', 'wait-dot'), node('span', 'activity-title', 'Clarification requested'));
      const subtitle = node('span', 'activity-subtitle', clarification.text);
      subtitle.title = clarification.text;
      row.append(line, subtitle);
      rows.push(row);
    }
    elements['wait-list'].replaceChildren(...rows);
    if (loading) elements['wait-list'].append(...skeletonRows(1));
    else if (!rows.length) elements['wait-list'].append(node('p', 'activity-empty', 'Nothing waiting for input'));
  }
  const groups = new Map(view?.loadedSessions);
  for (const task of data?.sessionTasks ?? []) groups.set(task.id, task);
  const active = data?.activeSessions ?? [];
  const more = elements['more-sessions'];
  more.hidden = !view?.sessionsBefore;
  more.disabled = !!view?.loadingSessions;
  if (!changed('sessions', view?.id, loading, [...groups.values()], active)) return;
  const key = (taskId, run) => JSON.stringify([taskId, run.environmentId, run.threadId]);
  const activeKeys = new Set();
  const live = [];
  for (const run of active) {
    const identity = key(run.taskId, run);
    if (activeKeys.has(identity)) continue;
    activeKeys.add(identity);
    live.push(sessionRow({ id: run.taskId, request: run.request }, run, stateLabels[run.state] ?? run.state, true));
  }
  const ended = [];
  for (const task of [...groups.values()].sort((a, b) => b.id - a.id)) {
    for (const session of task.sessions) {
      if (!activeKeys.has(key(task.id, session))) ended.push(sessionRow(task, session, 'Ended', false));
    }
  }
  const rows = [];
  for (const [label, items] of [['Active', live], ['Ended', ended]]) {
    if (!items.length) continue;
    const heading = node('h3', 'activity-group');
    heading.append(node('span', null, label), node('span', 'activity-group-count', items.length));
    rows.push(heading, ...items);
  }
  const list = elements['session-list'];
  list.setAttribute('aria-busy', loading);
  list.replaceChildren(...rows);
  if (loading) list.append(...skeletonRows(2));
  else if (!rows.length) list.append(node('p', 'activity-empty', 'No sessions yet'));
}

function beginReply(target, text = '') {
  if (!target.messageId || sendingMessages.has(view.id)) return;
  if (view.dm) selectBot(view.id);
  const current = replies.get(view.id);
  if (current?.messageId !== target.messageId || text)
    replies.set(view.id, { ...target, text });
  closeDetail();
  renderComposer();
  elements.message.focus();
}
const icons = {
  copy: '<path d="M9 9.5A1.5 1.5 0 0 1 10.5 8h7A1.5 1.5 0 0 1 19 9.5v9a1.5 1.5 0 0 1-1.5 1.5h-7A1.5 1.5 0 0 1 9 18.5z"/><path d="M15 8V5.5A1.5 1.5 0 0 0 13.5 4h-7A1.5 1.5 0 0 0 5 5.5v9A1.5 1.5 0 0 0 6.5 16H9"/>',
  copied: '<path d="m5.5 12.5 4 4 9-9"/>',
  reply: '<path d="M9.5 7 4.5 12l5 5"/><path d="M5 12h9a5.5 5.5 0 0 1 5.5 5.5V19"/>',
  check: '<circle cx="12" cy="12" r="8.5"/><path d="m8.5 12.3 2.4 2.4 4.6-4.9"/>',
  clock: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/>',
  open: '<path d="M10 5H6.5A1.5 1.5 0 0 0 5 6.5v11A1.5 1.5 0 0 0 6.5 19h11a1.5 1.5 0 0 0 1.5-1.5V14"/><path d="M14 5h5v5M19 5l-8 8"/>',
};
function icon(name, className = 'glyph') {
  const host = node('span', className);
  host.setAttribute('aria-hidden', 'true');
  host.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${icons[name]}</svg>`;
  return host;
}
function iconButton(name, label, click) {
  const control = button(null, 'icon-button msg-action', click);
  control.setAttribute('aria-label', label);
  control.title = label;
  control.append(icon(name));
  return control;
}
async function copyMessage(control, text) {
  try {
    await navigator.clipboard.writeText(text);
    control.replaceChildren(icon('copied'));
    control.setAttribute('aria-label', 'Copied');
    control.title = 'Copied';
    setTimeout(() => {
      control.replaceChildren(icon('copy'));
      control.setAttribute('aria-label', 'Copy text');
      control.title = 'Copy text';
    }, 1600);
  } catch { if (view) setNotice(view, 'Could not copy the text.', true); }
}
function questionActions(context, task, { dialog = false } = {}) {
  const target = { kind: 'wait', messageId: task.wait.messageId, summary: waitText(task.wait) };
  const actions = node('div', dialog ? 'wait-primary' : 'event-actions question-actions');
  for (const choice of task.wait.choices ?? []) {
    const option = button(choice, dialog ? 'dialog-button' : 'button', () => {
      if (view?.id === context.id) beginReply(target, choice);
    });
    option.disabled = pendingWaits.has(task.wait.id);
    actions.append(option);
  }
  const reply = button('Write a reply', dialog ? 'dialog-button quiet' : 'text-button', () => {
    if (view?.id === context.id) beginReply(target);
  });
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
function messageSide(event) {
  if (event.kind === 'cancelled') return 'system';
  return ['conversation_message', 'user_answer'].includes(event.kind) || (event.kind === 'request' && event.data.source !== 'cron') ? 'user' : 'bot';
}
function reasonText(text) {
  const reason = node('p', 'event-reason');
  reason.append(node('span', 'sr-only', 'Reason: '), document.createTextNode(text));
  return reason;
}
function eventTime(event, extra) {
  const timestamp = new Date(event.time * 1000);
  const time = node('time', 'msg-time', clock.format(timestamp));
  time.dateTime = timestamp.toISOString();
  time.title = [event.taskId ? `#${event.taskId}` : null, fullTime.format(timestamp), extra].filter(Boolean).join(' · ');
  return time;
}
function sourceAction(source, label = 'Open source', botId = view?.id) {
  return button(label, 'text-button', async () => {
    if (!source.messageId && source.taskId) {
      await openTask(source.taskId, botId);
      return;
    }
    const target = botId === view.id ? byId(`event-${source.messageId}`) : null;
    if (target) {
      closeDetail();
      target.focus();
      target.scrollIntoView({ block: 'center' });
      return;
    }
    const selection = { kind: 'source', botId };
    openDetail(selection);
    try {
      const page = await api(botQuery('conversation', botId, source.messageId + 1));
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
function eventCard(event, context, place) {
  const data = event.data;
  const bot = context.bot;
  const side = messageSide(event);
  if (side === 'system') {
    const note = node('article', 'system-note', `${bot?.name ?? 'Bot'} · Task #${event.taskId} cancelled · `);
    note.id = `event-${event.id}`;
    note.tabIndex = -1;
    note.append(eventTime(event));
    return note;
  }
  const row = node('article', `msg ${side}`);
  row.id = `event-${event.id}`;
  row.tabIndex = -1;
  row.classList.toggle('group-start', place.groupStart);
  row.classList.toggle('task-start', place.taskStart);
  const cron = event.kind === 'request' && data.source === 'cron';
  const scheduled = cron && data.scheduledAt ? `Scheduled for ${fullTime.format(data.scheduledAt * 1000)}` : '';
  const showMeta = side === 'bot' ? place.groupStart || place.taskStart : place.taskStart;
  const meta = node('div', showMeta ? 'msg-meta' : 'msg-meta sr-only');
  const name = node('span', side === 'bot' && place.groupStart ? 'msg-name' : 'msg-name sr-only',
    side === 'bot' ? bot?.name ?? 'Bot' : 'You');
  const task = node('span', place.taskStart ? 'msg-task' : 'msg-task sr-only', event.taskId ? `#${event.taskId}` : '');
  meta.append(name, task, eventTime(event, scheduled));
  if (event.taskId) meta.append(button('Task details', 'text-button event-task', () => void openTask(event.taskId)));
  row.append(meta);

  const line = node('div', 'msg-line');
  if (side === 'bot') line.append(place.groupEnd && bot ? avatar(bot, 'msg-avatar') : node('span', 'msg-avatar-space'));
  const bubble = node('div', 'bubble');
  const text = node('p', 'event-text');
  const actions = node('div', 'msg-actions');
  if (!showMeta) {
    const hoverTime = eventTime(event, scheduled);
    hoverTime.setAttribute('aria-hidden', 'true');
    actions.append(hoverTime);
  }
  const extras = [];
  let copy = data.text ?? '';
  const openTask = event.kind === 'waiting' ? context.waits.get(data.id) : null;

  if (event.kind === 'waiting') {
    copy = waitText(data);
    text.textContent = copy;
    bubble.append(text);
    if (data.decisionReason) bubble.append(reasonText(data.decisionReason));
    if (openTask?.wait.reason === 'user_question') extras.push(questionActions(view, openTask));
    else if (openTask) {
      const open = node('div', 'event-actions question-actions');
      open.append(button('View pending request', 'button', () => openDetail({ kind: 'wait', taskId: openTask.id, waitId: data.id })));
      extras.push(open);
    } else {
      const resolution = node('p', 'msg-resolution');
      resolution.append(icon('check'), document.createTextNode(closedLabels[context.closed.get(data.id)] ?? 'Request closed'));
      extras.push(resolution);
    }
  } else if (event.kind === 'conversation_reply') {
    text.textContent = data.text;
    bubble.append(text);
    const clarification = context.clarifications.get(data.messageId);
    if (clarification) extras.push(clarificationActions(clarification));
    if (data.source?.messageId) {
      const links = node('div', 'event-actions');
      links.append(sourceAction(data.source));
      if (data.route === 'respond') links.append(button('Reply to this answer', 'text-button', () =>
        beginReply({ kind: 'result', messageId: event.id, summary: data.text })));
      extras.push(links);
    }
  } else if (event.kind === 'report') {
    extras.push(resultActions(event, data.report));
    copy = data.report || 'The session submitted a report.';
    text.textContent = copy;
    const link = linkToRun(data.run);
    if (link) {
      link.className = 'run-link';
      link.title = 'Open in T3';
      link.setAttribute('aria-label', 'Open in T3');
      link.replaceChildren(icon('open'));
      text.append(link);
    }
    bubble.append(text);
    if (data.status && data.status !== 'completed') {
      bubble.append(node('p', 'event-reason', `Run status: ${stateLabels[data.status] ?? data.status}`));
    }
    if (data.truncated) bubble.append(node('p', 'event-reason', 'Open T3 to read the full report.'));
  } else if (event.kind === 'completed') {
    extras.push(resultActions(event, data.text, data.run));
    bubble.classList.add('completed');
    text.append(icon('check', 'glyph kind-icon'), node('span', 'sr-only', 'Complete: '), document.createTextNode(data.text ?? ''));
    bubble.append(text);
    if (data.reason) bubble.append(reasonText(data.reason));
  } else {
    if (cron) text.append(icon('clock', 'glyph kind-icon'), node('span', 'sr-only', 'Recurring request: '));
    text.append(document.createTextNode(data.text ?? ''));
    bubble.append(text);
    if (event.kind === 'user_answer') {
      const question = data.question;
      if (question && !place.afterQuestion) {
        const quote = node('p', 'msg-quote');
        quote.append(icon('reply'), node('span', null, question));
        quote.title = question;
        row.insertBefore(quote, meta.nextSibling);
      }
    }
    if (event.kind === 'conversation_message' && data.replyToMessageId)
      extras.push(sourceAction({ messageId: data.replyToMessageId }, 'Open original message'));
    if (data.status === 'received') extras.push(node('p', 'msg-note', 'Received'));
    if (data.deliveredToT3) extras.push(node('p', 'msg-note', 'Sent to T3'));
    const blocker = ['request', 'conversation_message'].includes(event.kind) ? context.queued.get(event.taskId)?.blockedBy : null;
    if (blocker) {
      const owner = bots.find((item) => item.id === blocker.botId)?.name ?? blocker.botId;
      const note = node('p', 'msg-note', `#${blocker.id}（${owner}）Waiting for completion · ${blocker.request}`);
      note.title = blocker.request;
      extras.push(note);
    }
  }
  if (copy) {
    const control = iconButton('copy', 'Copy text', () => void copyMessage(control, copy));
    actions.append(control);
  }
  if (openTask?.wait.reason === 'user_question') {
    const reply = iconButton('reply', 'Write a reply', () => beginReply({ kind: 'wait', messageId: openTask.wait.messageId, summary: waitText(openTask.wait) }));
    reply.disabled = pendingWaits.has(openTask.wait.id);
    actions.append(reply);
  }
  line.append(bubble, actions);
  row.append(line, ...extras);
  return row;
}
function workingLine(data) {
  const row = node('div', 'msg bot working group-start');
  const line = node('div', 'msg-line');
  const dots = node('span', 'typing');
  dots.setAttribute('aria-hidden', 'true');
  dots.append(node('span'), node('span'), node('span'));
  let label = presenceLabels.thinking;
  if (data.presence === 'working') {
    const active = data.activeSessions?.[0];
    label = data.sessionTasks?.find((task) => task.id === active?.taskId)?.request ?? presenceLabels.working;
  }
  const text = node('span', 'working-label', label);
  text.title = label;
  line.append(avatar(data.bot, 'msg-avatar'), dots, text);
  row.append(line);
  return row;
}
function dayLabel(seconds) {
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const days = Math.round((today - new Date(seconds * 1000).setHours(0, 0, 0, 0)) / 86400000);
  return days === 0 ? 'Today' : days === 1 ? 'Yesterday' : dayFormat.format(seconds * 1000);
}

function peerAuthors(data) {
  const from = data.initiator === 'user' ? 'You' : participant(data.fromBotId).name;
  const to = data.recipient === 'user' ? 'You' : participant(data.toBotId).name;
  return `${from} → ${to}`;
}
function peerConversation(messages, activeBot) {
  const first = messages[0];
  const { fromBotId, toBotId } = first.data;
  const senderBot = fromBotId ?? first.data.contextBotId;
  const peerId = senderBot === activeBot ? toBotId : senderBot;
  const peerName = participant(peerId).name;
  const entry = node('div', 'peer-exchange-entry');
  const group = node('details', 'peer-exchange');
  group.dataset.firstMessage = first.id;
  const summary = node('summary');
  const count = messages.length === 1 ? '1 message' : `${messages.length} messages`;
  summary.append(node('span', null, `${count} with ${peerName}`));
  if (first.taskId) summary.append(node('span', 'peer-exchange-task', `#${first.taskId}`));
  const list = node('ol', 'peer-message-list');
  list.setAttribute('role', 'list');
  for (const message of messages) {
    const { messageType, requestKind, text, evidence } = message.data;
    const item = node('li', 'peer-message');
    const metadata = node('div', 'event-meta');
    const label = messageType === 'request' && requestKind === 'consult'
      ? 'Consultation' : peerMessageLabels[messageType] ?? 'Message';
    metadata.append(node('strong', null, peerAuthors(message.data)),
      node('span', null, label), eventTime(message));
    item.append(metadata, node('p', 'event-text', text));
    for (const reference of evidence ?? []) {
      const link = linkToRun(reference, 'View evidence in T3');
      if (link) item.append(link);
    }
    list.append(item);
  }
  group.append(summary, list);
  entry.append(group);
  if (peerId) {
    const link = peerLink(activeBot, peerId, 'Open DM', 'link peer-exchange-open');
    link.setAttribute('aria-label', `Open DM with ${peerName}`);
    link.dataset.peerFocus = `exchange:${first.id}`;
    entry.append(link);
  }
  return entry;
}

function peerThread(events, dm) {
  const fragment = document.createDocumentFragment();
  if (!events.length) {
    const empty = node('div', 'empty-state');
    const title = dm.error ? 'DM unavailable' : dm.loaded ? 'No messages yet' : 'Loading…';
    empty.append(node('h2', null, title));
    if (dm.error) empty.append(node('p', null, dm.error));
    else if (dm.canSend) empty.append(node('p', null, `Send a message to ${participant(dm.peerId).name}.`));
    fragment.append(empty);
  }
  let day = '';
  for (const event of events) {
    const nextDay = dayLabel(event.time);
    if (nextDay !== day) {
      fragment.append(node('p', 'day', nextDay));
      day = nextDay;
    }
    const data = event.data;
    const fromUser = data.initiator === 'user';
    const row = node('article', `msg group-start peer-dm-message ${fromUser ? 'user' : 'bot'}`);
    const metadata = node('div', 'msg-meta');
    metadata.append(node('span', 'msg-name', peerAuthors(data)), eventTime(event));
    if (['question', 'failed', 'cancelled'].includes(data.messageType))
      metadata.append(node('span', null, peerMessageLabels[data.messageType]));
    const line = node('div', 'msg-line');
    if (!fromUser) line.append(avatar(participant(data.fromBotId), 'msg-avatar'));
    const bubble = node('div', 'bubble');
    bubble.append(node('p', 'event-text', data.text));
    const actions = node('div', 'peer-dm-sources');
    if (event.taskId) {
      const link = button(`Task #${event.taskId}`, 'text-button',
        () => void openTask(event.taskId, event.taskBotId));
      link.dataset.peerFocus = `task:${event.id}`;
      actions.append(link);
    }
    for (const [index, reference] of (data.evidence ?? []).entries()) {
      const link = linkToRun(reference, 'Evidence');
      if (link) {
        link.dataset.peerFocus = `evidence:${event.id}:${index}`;
        actions.append(link);
      }
    }
    line.append(bubble);
    row.append(metadata, line);
    if (actions.childElementCount) row.append(actions);
    fragment.append(row);
  }
  return fragment;
}

function timelineScrollPosition() {
  const scroller = elements.conversation;
  const height = scroller.scrollHeight;
  const top = scroller.scrollTop;
  return { height, top, atBottom: height - top - scroller.clientHeight < 65 };
}

function renderTimeline(scroll = 'auto', previousScroll = timelineScrollPosition()) {
  const older = elements.older;
  const data = view?.data;
  const context = composerContext();
  const events = [...(context?.events.values() ?? [])].sort((a, b) => a.id - b.id);
  older.hidden = !context?.olderBefore;
  older.disabled = !!context?.loadingOlder;
  older.textContent = context?.loadingOlder ? 'Loading…' : 'Show earlier conversation';
  const scroller = elements.conversation;
  const focused = elements.timeline.contains(document.activeElement) ? document.activeElement : null;
  const focusedLink = focused?.dataset.peerFocus;
  const moveScroll = () => {
    if (focusedLink) {
      const link = [...elements.timeline.querySelectorAll('[data-peer-focus]')].find((item) => item.dataset.peerFocus === focusedLink);
      link?.focus({ preventScroll: true });
    }
    if (scroll === 'older') scroller.scrollTop = previousScroll.top + scroller.scrollHeight - previousScroll.height;
    else if (scroll === 'bottom' || previousScroll.atBottom) scroller.scrollTop = scroller.scrollHeight;
    else scroller.scrollTop = previousScroll.top;
  };
  if (view?.dm) {
    const dm = view.dm;
    const participants = [participant(dm.id), participant(dm.peerId)].map(({ id, name, avatar }) => [id, name, avatar]);
    if (!changed('timeline', composerKey(dm), events, dm.loaded, dm.canSend, dm.error, participants, dayFormat.format(Date.now()))) return;
    scroller.setAttribute('aria-label', 'Bot DM history');
    elements.timeline.replaceChildren(peerThread(events, dm));
    moveScroll();
    return;
  }
  if (!changed('timeline', view?.id, data?.bot, events, data?.waits, data?.queued, [...pendingWaits], listLoaded,
    data?.presence, data?.activeSessions, data?.sessionTasks, data?.clarifications, bots, dayFormat.format(Date.now()))) return;
  scroller.setAttribute('aria-label', 'Conversation history');
  const expanded = new Set([...elements.timeline.querySelectorAll('.peer-exchange[open]')].map(group => group.dataset.firstMessage));
  const focusedExchange = focused?.closest('.peer-exchange > summary')?.parentElement.dataset.firstMessage;
  let focusTarget = null;
  const timeline = {
    bot: data?.bot ?? bots.find((item) => item.id === view?.id),
    clarifications: new Map((data?.clarifications ?? []).map(item => [item.messageId, item])),
    waits: new Map((data?.waits ?? []).map((task) => [task.wait.id, task])),
    queued: new Map((data?.queued ?? []).map((task) => [task.id, task])),
    closed: new Map(events.filter((event) => event.kind === 'wait_closed').map((event) => [event.data.waitId, event.data.resolution])),
  };
  const shown = events.filter((event) => event.kind !== 'wait_closed');
  const fragment = document.createDocumentFragment();
  let peerMessages = [];
  const appendPeerMessages = () => {
    if (!peerMessages.length) return;
    const entry = peerConversation(peerMessages, view.id);
    const group = entry.querySelector('.peer-exchange');
    group.open = expanded.has(group.dataset.firstMessage);
    if (focusedExchange === group.dataset.firstMessage) focusTarget = group.querySelector('summary');
    fragment.append(entry);
    peerMessages = [];
  };
  let day = '';
  let previous = null;
  shown.forEach((event, index) => {
    const eventDay = dayLabel(event.time);
    if (eventDay !== day || event.kind !== 'bot_message' || peerMessages[0]?.data.exchangeId !== event.data.exchangeId) appendPeerMessages();
    if (eventDay !== day) {
      fragment.append(node('p', 'day', eventDay));
      day = eventDay;
      previous = null;
    }
    if (event.kind === 'bot_message') { peerMessages.push(event); previous = null; return; }
    const side = messageSide(event);
    const next = shown[index + 1];
    const place = {
      groupStart: previous?.side !== side,
      groupEnd: !next || next.kind === 'bot_message' || messageSide(next) !== side || dayLabel(next.time) !== day,
      taskStart: previous?.taskId !== event.taskId,
      afterQuestion: previous?.kind === 'waiting' && previous.waitId === event.data.waitId,
    };
    fragment.append(eventCard(event, timeline, place));
    previous = { side, taskId: event.taskId, kind: event.kind, waitId: event.data.id };
  });
  appendPeerMessages();
  if (!fragment.childElementCount) {
    const empty = node('div', 'empty-state');
    if (data?.bot) empty.append(avatar(data.bot));
    const loaded = !!data || (!view && listLoaded);
    empty.append(node('h2', null, loaded ? (data ? data.bot.name : 'No Bots') : 'Loading…'));
    if (data) empty.append(node('p', null, 'Send a message to start the conversation.'));
    fragment.append(empty);
  }
  if (data && (data.presence === 'working' || data.presence === 'thinking')) fragment.append(workingLine(data));
  elements.timeline.replaceChildren(fragment);
  focusTarget?.focus({ preventScroll: true });
  moveScroll();
}

const sendKey = /mac|iphone|ipad/i.test(navigator.userAgentData?.platform ?? navigator.platform) ? '⌘' : 'Ctrl';
function setNotice(context, text, isError = false) {
  const key = composerKey(context);
  notices.set(key, { text, isError });
  if (showingComposer(key)) renderComposer();
}
const replyLabels = { wait: 'Reply to a question', clarification: 'Reply to a clarification', result: 'Reply to a result' };
const composerLimit = (reply) => reply?.kind === 'wait' ? 8000 : 16000;
function scheduleNoticeFade(context, notice) {
  const key = composerKey(context);
  if (!notice || notice.isError || notice.timer || sendingMessages.has(key) || [...pendingWaits.values()].includes(key)) return;
  notice.timer = setTimeout(() => {
    notice.timer = null;
    if (notices.get(key) !== notice || !showingComposer(key)) return;
    notice.fading = true;
    renderComposer();
    setTimeout(() => {
      if (notices.get(key) !== notice) return;
      notices.delete(key);
      if (showingComposer(key)) renderComposer();
    }, 400);
  }, 3600);
}
function fitMessage() {
  const field = elements.message;
  if (!field.offsetParent) return;
  field.style.height = 'auto';
  field.style.height = `${field.scrollHeight}px`;
}
function renderComposer() {
  const context = composerContext();
  const dm = view?.dm;
  const key = context && composerKey(context);
  const notice = notices.get(key);
  const readOnly = elements['peer-read-only'];
  const peerName = dm && participant(dm.peerId).name;
  const readOnlyText = dm?.loaded && !dm.canSend ? `Read only · Messaging ${peerName} from this Bot is restricted.` : '';
  readOnly.hidden = !readOnlyText;
  readOnly.textContent = readOnlyText && (notice?.isError ? notice.text : readOnlyText);
  const available = dm ? dm.canSend : !!context?.data;
  elements['composer-container'].hidden = !available;
  if (!available) return;
  const reply = replies.get(key);
  const body = reply ? reply.text : drafts.get(key) ?? '';
  const busy = sendingMessages.has(key);
  elements['reply-context'].hidden = !reply;
  elements['reply-label'].textContent = reply ? replyLabels[reply.kind] : '';
  elements['reply-question'].textContent = reply?.summary ?? '';
  elements['compose-label'].textContent = reply ? replyLabels[reply.kind] : dm ? `Message ${peerName}` : 'Message';
  elements['cancel-reply'].disabled = busy;
  elements.message.placeholder = reply ? 'Write a reply' : `Message ${peerName || context.data.bot.name}`;
  if (elements.message.value !== body) {
    elements.message.value = body;
    fitMessage();
  }
  elements.message.readOnly = busy;
  elements.composer.setAttribute('aria-busy', String(busy));
  elements.send.disabled = busy || !body.trim() || byteLength(body) > composerLimit(reply);
  elements.composer.classList.toggle('can-send', !elements.send.disabled);
  elements.send.setAttribute('aria-label', reply ? 'Send reply' : dm ? `Send message to ${peerName}` : 'Send message');
  elements.send.title = `${reply ? 'Reply' : 'Send'} (${sendKey}+Enter)`;
  elements.notice.textContent = notice?.text ?? '';
  elements.notice.classList.toggle('error', !!notice?.isError);
  elements.notice.classList.toggle('fading', !!notice?.fading);
  scheduleNoticeFade(context, notice);
}

const detailNav = byId('detail-nav');
const settingsSections = {
  general: ['General', '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="4.5" y="6" width="15" height="12.5" rx="4"/><path d="M12 6V3.5M9.5 12.2v.6M14.5 12.2v.6"/></svg>'],
  cron: ['Recurring requests', '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="8"/><path d="M12 7.5V12l3 2"/></svg>'],
};
const chevron = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9.5 6.5 5.5 5.5-5.5 5.5"/></svg>';
let detailAvatar = null;
let detailOrigin = null;

function releaseDetailAvatar() {
  detailAvatar?.handle?.destroy();
  detailAvatar = null;
}
function closeDetail() {
  detail = null;
  releaseDetailAvatar();
  if (elements.detail.open) elements.detail.close();
}
function openDetail(selection) {
  if (!view || (!view.data && selection.kind !== 'task' && selection.kind !== 'source')) return;
  detail = selection.kind === 'settings' ? { section: 'general', ...selection } : selection;
  signatures.delete('detail');
  renderDetail();
  if (elements.detail.open) return;
  detailOrigin = document.activeElement;
  elements.detail.showModal();
  if (!detailNav.hidden) detailNav.querySelector('[aria-selected="true"]')?.focus();
}
async function openTask(taskId, botId = view?.id) {
  if (!view || !botId) return;
  const selection = { kind: 'task', taskId, botId };
  openDetail(selection);
  try {
    const task = await api(`task?${new URLSearchParams({ bot: botId, taskId })}`);
    if (detail !== selection) return;
    selection.task = task;
  } catch {
    if (detail !== selection) return;
    selection.failed = true;
  }
  renderDetail();
}
function taskContent(task, botId) {
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
      if (revision.sourceMessageId) row.append(sourceAction({ messageId: revision.sourceMessageId }, 'Open change request', botId));
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
    actions.append(sourceAction({ messageId: task.result.id }, 'Open report in conversation', botId));
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
      sources.append(sourceAction(source, source.messageId ? 'Open original result' : 'Open original task', botId));
    }
    if (!task.sourceResultRefs.length)
      sources.append(button('Open original task', 'text-button', () => void openTask(task.sourceTaskId, botId)));
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
elements.detail.addEventListener('close', () => {
  if (elements.detail.open) return;
  releaseDetailAvatar();
  const origin = detailOrigin;
  detailOrigin = null;
  const active = document.activeElement;
  if (origin?.isConnected && (active === document.body || elements.detail.contains(active))) origin.focus();
});

const weekdayNames = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const cronNames = { sun: 0, mon: 1, tue: 2, wed: 3, thu: 4, fri: 5, sat: 6 };
function cronValues(field, min, max, names = {}) {
  const value = (text) => {
    const number = /^\d+$/.test(text) ? Number(text) : names[text.toLowerCase()];
    return number >= min && number <= max ? number : null;
  };
  const values = new Set();
  for (const part of field.split(',')) {
    const [from, to, extra] = part.split('-').map(value);
    if (extra !== undefined || from == null || (part.includes('-') && (to == null || to < from))) return null;
    for (let item = from; item <= (to ?? from); item += 1) values.add(item);
  }
  return [...values].sort((a, b) => a - b);
}
function describeCron(expression) {
  const fields = String(expression).trim().split(/\s+/);
  if (fields.length !== 5) return null;
  const [minute, hour, day, month, weekday] = fields;
  const interval = (field, cycle) => {
    const step = Number(/^\*\/([1-9]\d?)$/.exec(field)?.[1]);
    return cycle % step === 0 ? step : null;
  };
  const daily = day === '*' && month === '*' && weekday === '*';
  if (daily && hour === '*' && minute === '*') return 'Every minute';
  const minuteInterval = interval(minute, 60);
  if (daily && hour === '*' && minuteInterval) return `Every ${minuteInterval} minutes`;
  if (!/^\d+$/.test(minute) || Number(minute) > 59) return null;
  const mm = minute.padStart(2, '0');
  if (daily && hour === '*') return `Hourly at :${mm}`;
  const hourInterval = interval(hour, 24);
  if (daily && hourInterval) return `Every ${hourInterval} hours${minute === '0' ? '' : ` at :${mm}`}`;
  const hours = cronValues(hour, 0, 23);
  if (!hours || hours.length > 4) return null;
  const times = hours.map((item) => `${item}:${mm}`).join(', ');
  if (day === '*' && month === '*') {
    if (weekday === '*') return `Daily at ${times}`;
    const days = cronValues(weekday, 0, 7, cronNames);
    if (!days) return null;
    const unique = [...new Set(days.map((item) => item % 7))].sort((a, b) => ((a + 6) % 7) - ((b + 6) % 7));
    const key = unique.join(',');
    if (unique.length === 7) return `Daily at ${times}`;
    if (key === '1,2,3,4,5') return `Weekdays at ${times}`;
    if (key === '6,0') return `Weekends at ${times}`;
    return `Every ${unique.map((item) => weekdayNames[item]).join(', ')} at ${times}`;
  }
  if (weekday !== '*') return null;
  const days = cronValues(day, 1, 31);
  if (!days || days.length > 4) return null;
  if (month === '*') return `Monthly on ${days.join(', ')} at ${times}`;
  const months = cronValues(month, 1, 12);
  if (!months || months.length !== 1 || days.length !== 1) return null;
  return `Yearly on ${months[0]}/${days[0]} at ${times}`;
}

function sectionsFor(bot) {
  return bot.cron.length ? ['general', 'cron'] : ['general'];
}
function selectSection(section, focus = false) {
  if (detail?.kind !== 'settings' || detail.section === section) return;
  detail.section = section;
  renderDetail();
  elements['detail-body'].scrollTop = 0;
  if (focus) byId(`detail-tab-${section}`)?.focus();
}
function renderDetailNav(sections, current) {
  const body = elements['detail-body'];
  elements.detail.classList.toggle('has-nav', sections.length > 1);
  detailNav.hidden = sections.length < 2;
  if (detailNav.hidden) {
    body.removeAttribute('role');
    body.removeAttribute('aria-labelledby');
    return;
  }
  if (changed('detail-nav', sections)) {
    detailNav.replaceChildren(...sections.map((section) => {
      const [label, icon] = settingsSections[section];
      const tab = button(null, 'detail-tab', () => selectSection(section));
      tab.id = `detail-tab-${section}`;
      tab.dataset.section = section;
      tab.setAttribute('role', 'tab');
      tab.setAttribute('aria-controls', 'detail-body');
      tab.innerHTML = icon;
      tab.append(node('span', null, label));
      return tab;
    }));
  }
  detailNav.setAttribute('aria-orientation', narrow.matches ? 'horizontal' : 'vertical');
  for (const tab of detailNav.children) {
    const selected = tab.dataset.section === current;
    tab.setAttribute('aria-selected', selected);
    tab.tabIndex = selected ? 0 : -1;
  }
  body.setAttribute('role', 'tabpanel');
  body.setAttribute('aria-labelledby', `detail-tab-${current}`);
}
detailNav.addEventListener('keydown', (event) => {
  const tabs = [...detailNav.children];
  const index = tabs.indexOf(document.activeElement);
  if (index < 0) return;
  const next = { ArrowDown: index + 1, ArrowRight: index + 1, ArrowUp: index - 1, ArrowLeft: index - 1,
    Home: 0, End: tabs.length - 1 }[event.key];
  if (next === undefined) return;
  event.preventDefault();
  selectSection(tabs[(next + tabs.length) % tabs.length].dataset.section, true);
});

function settingsRow(label, value, click) {
  const row = click ? button(null, 'settings-row', click) : node('div', 'settings-row');
  row.append(node('span', 'settings-row-label', label), node('span', 'settings-row-value', value));
  if (click) row.insertAdjacentHTML('beforeend', chevron);
  return row;
}
function generalContent(bot) {
  const presence = view.data.presence ?? 'idle';
  const hero = node('div', 'settings-hero');
  const face = node('span', 'settings-avatar');
  detailAvatar = { state: presence, mount: () => {
    const handle = showAvatar(face, bot, { still: false, state: presence });
    hero.style.setProperty('--hero-tint', accentColor(bot));
    return handle;
  } };
  hero.append(face, node('p', 'settings-name', bot.name));
  const role = node('section', 'settings-card');
  role.setAttribute('aria-label', 'Role');
  role.append(bot.soul ? node('p', 'settings-soul', bot.soul) : node('p', 'settings-empty', 'No role configured.'));
  const schedules = node('div', 'settings-card settings-rows');
  schedules.append(bot.cron.length
    ? settingsRow('Recurring requests', `${bot.cron.length} scheduled`, () => selectSection('cron', true))
    : settingsRow('Recurring requests', 'None'));
  return [hero, role, schedules];
}
function cronContent(bot) {
  const list = node('div', 'settings-card cron-list');
  for (const item of bot.cron) {
    const expression = String(item.schedule ?? item.expression ?? item.cron ?? '');
    const timezone = item.timezone ?? item.timeZone;
    const readable = describeCron(expression);
    const row = node('div', `cron-row${item.enabled === false ? ' disabled' : ''}`);
    const when = node('p', 'cron-when');
    when.append(readable ? node('span', 'cron-schedule', readable) : node('code', 'cron-expression', expression || '—'));
    if (timezone) when.append(node('span', 'cron-zone', timezone));
    if (readable) when.append(node('code', 'cron-expression', expression));
    row.append(when, node('span', 'cron-state', item.enabled === false ? 'Paused' : 'Enabled'),
      node('p', 'cron-request', item.request));
    list.append(row);
  }
  return [list];
}
function settingsContent(bot, section = 'general') {
  elements['detail-title'].textContent = settingsSections[section][0];
  return section === 'cron' ? cronContent(bot) : generalContent(bot);
}
function waitForm(label, fields, submit, id) {
  const form = node('form', 'wait-form');
  if (id) form.id = id;
  for (const field of fields) {
    const wrapper = node('label', null, field.label);
    const input = node('input');
    Object.assign(input, { name: field.name, type: field.type ?? 'text', value: field.value ?? '', required: field.required ?? false });
    wrapper.append(input);
    form.append(wrapper);
  }
  const action = button(label, 'dialog-button');
  action.type = 'submit';
  if (id) action.setAttribute('form', id);
  else form.append(action);
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    submit(Object.fromEntries(new FormData(form)));
  });
  return [form, action];
}
function localDateTime(timestamp) {
  const date = new Date(timestamp);
  return new Date(timestamp - date.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
}
function waitContent(context, task) {
  const wait = task.wait;
  elements['detail-title'].textContent = waitLabel(wait.reason);
  const request = node('blockquote', 'wait-request');
  const marker = node('span', 'wait-task', `#${task.id}`);
  marker.setAttribute('aria-label', `Task #${task.id}`);
  request.append(node('p', 'wait-request-text', task.request), marker);
  const reason = node('div', 'wait-reason');
  reason.append(node('p', 'wait-text', waitText(wait)));
  if (wait.decisionReason) reason.append(node('p', 'wait-why', wait.decisionReason));
  const content = [request, reason];
  const actions = node('div', 'wait-actions');
  const primary = node('div', 'wait-primary');
  const perform = (operation, fields) => void operate(context, task, operation, fields);
  const action = (label, operation) => button(label, 'dialog-button primary', () => perform(operation));
  const runLink = linkToRun(wait.run, wait.reason === 't3_input' ? 'Approve in T3' : 'Open in T3');
  if (runLink) {
    runLink.className = wait.reason === 't3_input' ? 'dialog-button primary' : 'dialog-button quiet';
    primary.append(runLink);
  }
  if (wait.reason === 'user_question') {
    primary.append(questionActions(context, task, { dialog: true }));
  } else if (wait.reason === 'budget') primary.append(action('Approve one more run', 'approve'));
  else if (wait.reason === 'model_failure') primary.append(action('Retry decision', 'retry'));
  else if (wait.reason === 'deadline') {
    if (task.deadline) reason.append(node('p', 'wait-why', `Current deadline · ${fullTime.format(task.deadline * 1000)}`));
    const [form, submit] = waitForm('Extend deadline', [{ name: 'deadline', label: 'New deadline (local time)',
      type: 'datetime-local', value: localDateTime(Date.now() + 60 * 60 * 1000), required: true }], ({ deadline }) => {
      const next = Math.floor(new Date(deadline).getTime() / 1000);
      if (next > Date.now() / 1000) perform('extend', { deadline: next });
      else showDetailError(errors.invalid_deadline);
    }, 'wait-deadline-form');
    submit.classList.add('primary');
    content.push(form);
    primary.append(submit);
  } else if (['unknown_delivery', 'auth', 'settings'].includes(wait.reason)) {
    primary.append(action('Reconcile', 'reconcile'));
    if (wait.reason === 'unknown_delivery') {
      const advanced = node('details', 'wait-advanced');
      advanced.append(node('summary', null, 'Specify the assigned run'), waitForm('Reconcile with this run', [
        { name: 'threadId', label: 'Thread ID', required: true }, { name: 'runId', label: 'Run ID', required: true },
        { name: 'questionId', label: 'Question ID (optional)' },
      ], (fields) => perform('reconcile', Object.fromEntries(Object.entries(fields).filter(([, value]) => value.trim()))))[0]);
      content.push(advanced);
    }
  }
  actions.append(button('Cancel task', 'wait-cancel', () => {
    if (window.confirm(`Cancel task #${task.id}?`)) perform('cancel');
  }), primary);
  const feedback = node('p', 'dialog-feedback');
  feedback.id = 'detail-feedback';
  feedback.setAttribute('role', 'status');
  return [...content, actions, feedback];
}
function showDetailError(text) {
  const output = byId('detail-feedback');
  if (output) output.textContent = text;
}
function labelDetail(label) {
  if (label) {
    elements.detail.setAttribute('aria-label', label);
    elements.detail.removeAttribute('aria-labelledby');
  } else {
    elements.detail.removeAttribute('aria-label');
    elements.detail.setAttribute('aria-labelledby', 'detail-title');
  }
}
function replaceDetail(render) {
  releaseDetailAvatar();
  elements['detail-body'].replaceChildren(...render());
  if (detailAvatar?.mount) detailAvatar.handle = detailAvatar.mount();
}
function renderDetail() {
  if (!detail || !view) return;
  if (detail.kind === 'settings') {
    const bot = view.data.bot;
    const sections = sectionsFor(bot);
    if (!sections.includes(detail.section)) detail.section = 'general';
    labelDetail(`${bot.name} settings`);
    renderDetailNav(sections, detail.section);
    if (changed('detail', detail, bot)) replaceDetail(() => settingsContent(bot, detail.section));
    const presence = view.data.presence ?? 'idle';
    if (detailAvatar && detailAvatar.state !== presence) {
      detailAvatar.handle?.update({ state: presence });
      detailAvatar.state = presence;
    }
    return;
  }
  labelDetail(null);
  renderDetailNav([], null);
  if (detail.kind === 'peers') {
    const peers = botPeers(view.id).map(({ id, canSend }) => ({ bot: participant(id), canSend }));
    if (changed('detail', detail, peers.map(({ bot, canSend }) => [bot.id, bot.name, bot.avatar, canSend]))) {
      const active = document.activeElement?.dataset.peerChoice;
      elements['detail-title'].textContent = 'Bot DMs';
      const list = node('ul', 'peer-picker-list');
      for (const { bot, canSend } of peers) {
        const item = node('li');
        const link = peerLink(view.id, bot.id, null, 'peer-choice');
        link.dataset.peerChoice = bot.id;
        link.append(avatar(bot, 'peer-choice-avatar'), node('span', 'peer-choice-name', bot.name));
        if (!canSend) link.append(node('span', 'peer-choice-status', 'Read only'));
        item.append(link);
        list.append(item);
      }
      replaceDetail(() => peers.length ? [list] : [node('p', 'prose', 'No Bot DMs available.')]);
      if (active) [...list.querySelectorAll('a')].find((link) => link.dataset.peerChoice === active)?.focus();
    }
    return;
  }
  if (detail.kind === 'task') {
    if (changed('detail', detail)) {
      elements['detail-title'].textContent = 'Task details';
      elements['detail-body'].replaceChildren(...(detail.task ? taskContent(detail.task, detail.botId) :
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
        button('Task details', 'text-button', () => void openTask(detail.event.taskId, detail.botId)));
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
      replaceDetail(() => [node('p', 'wait-why', 'Check the conversation for the latest update.')]);
    }
    return;
  }
  if (changed('detail', detail, task)) replaceDetail(() => waitContent(view, task));
  for (const control of elements['detail-body'].querySelectorAll('button, input')) control.disabled = pendingWaits.has(task.wait.id);
}
function render(scroll) {
  renderBots();
  renderHeader();
  renderActivity();
  const previousScroll = timelineScrollPosition();
  renderComposer();
  renderTimeline(scroll, previousScroll);
  renderDetail();
}

function mergeLatest(context, latest) {
  const { events, olderBefore } = latest;
  const first = events[0]?.id;
  const missedPage = first && first > context.newestEvent;
  if (!context.events.size || missedPage) {
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
async function refreshPeerConversation(context, scroll) {
  const dm = context.dm;
  const generation = ++dm.generation;
  try {
    const page = await api(peerQuery(dm));
    if (context !== view || generation !== dm.generation) return;
    const first = page.messages[0]?.id;
    if (!dm.events.size || first > dm.newestEvent) {
      dm.events.clear();
      dm.olderBefore = page.hasOlder ? first : 0;
    }
    for (const event of page.messages) dm.events.set(event.id, event);
    dm.newestEvent = page.messages.at(-1)?.id ?? dm.newestEvent;
    dm.loaded = true;
    dm.canSend = page.canSend;
    dm.error = null;
  } catch (error) {
    if (context !== view || generation !== dm.generation) return;
    if (error.status >= 400 && error.status < 500) dm.canSend = false;
    if (!dm.loaded || !dm.canSend) dm.error = errors[error.message] ?? 'Could not load this conversation.';
  }
  render(scroll);
}
async function refreshView(context, scroll) {
  const generation = ++context.generation;
  const peerRefresh = context.dm && refreshPeerConversation(context, scroll);
  try {
    const latest = await api(botQuery('view', context.id));
    if (context !== view || generation !== context.generation) return;
    mergeLatest(context, latest);
    setConnection('ready');
    render(scroll);
    if (context.openSettingsOnLoad) {
      const botId = context.openSettingsOnLoad;
      context.openSettingsOnLoad = null;
      openSettingsFromMenu(botId);
    }
  } catch (error) {
    if (context !== view || generation !== context.generation) return;
    if (error.status === 404) void refreshBots();
    else setConnection('offline');
  } finally {
    await peerRefresh;
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
    if (!bots.some((bot) => bot.id === view?.id)) selectLocation();
    render();
  } catch {
    if (generation === listGeneration) setConnection('offline');
  }
}
async function loadOlder() {
  const context = composerContext();
  const cursor = context?.olderBefore;
  if (!cursor || context.loadingOlder) return;
  context.loadingOlder = true;
  renderTimeline();
  try {
    const page = await api(context.peerId ? peerQuery(context, cursor) : botQuery('conversation', context.id, cursor));
    if (context !== composerContext() || context.olderBefore !== cursor) return;
    const events = context.peerId ? page.messages : page.events;
    context.olderBefore = context.peerId ? (page.hasOlder ? events[0]?.id : 0) : page.olderBefore;
    for (const event of events) context.events.set(event.id, event);
    renderTimeline('older');
  } catch { if (context === composerContext()) setNotice(context, 'Could not load earlier messages.', true); }
  finally {
    context.loadingOlder = false;
    if (context === composerContext()) renderTimeline();
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
  pendingWaits.set(waitId, context.id);
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
  const context = composerContext();
  if (!context || elements['composer-container'].hidden || elements.send.disabled) return;
  const key = composerKey(context);
  const reply = replies.get(key);
  const text = reply ? reply.text : drafts.get(key);
  const replyToMessageId = reply?.messageId;
  const previous = pendingMessage(context)?.payload;
  const clientMessageId = previous?.text === text && previous.replyToMessageId === replyToMessageId
    ? previous.clientMessageId : crypto.randomUUID();
  const payload = { botId: context.id, clientMessageId, text };
  if (replyToMessageId) payload.replyToMessageId = replyToMessageId;
  if (context.peerId) payload.peerId = context.peerId;
  rememberMessage(context, { payload, reply });
  sendingMessages.add(key);
  setNotice(context, 'Sending message…');
  try {
    await api(context.peerId ? 'peer-messages' : 'messages', payload);
    rememberMessage(context, null);
    if (reply) replies.delete(key);
    else drafts.delete(key);
    setNotice(context, 'Message received.');
    if (showingComposer(key)) await refreshView(view, 'bottom');
    void refreshBots();
  } catch (error) {
    if (error.status >= 400 && error.status < 500) rememberMessage(context, null);
    setNotice(context, errors[error.message] ?? 'Flock could not confirm receipt. You can resend the same message.', true);
  }
  finally {
    sendingMessages.delete(key);
    if (showingComposer(key)) renderComposer();
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
elements['peer-picker'].addEventListener('click', () => openDetail({ kind: 'peers' }));
elements['peer-back'].addEventListener('click', () => {
  selectBot(view.id);
  const picker = elements['peer-picker'];
  (picker.hidden ? elements.profile : picker).focus({ preventScroll: true });
});
elements['close-detail'].addEventListener('click', closeDetail);
elements.detail.addEventListener('close', () => { if (!elements.detail.open) detail = null; });
elements.detail.addEventListener('close', () => {
  if (elements.detail.open || !settingsReturnFocus) return;
  if (narrow.matches) openNavigation(true, false);
  focusKey(`menu:${settingsReturnFocus}`);
  settingsReturnFocus = null;
});
elements.older.addEventListener('click', loadOlder);
elements['more-sessions'].addEventListener('click', loadSessions);
elements.composer.addEventListener('submit', submit);
elements['cancel-reply'].addEventListener('click', () => {
  replies.delete(composerKey(composerContext()));
  renderComposer();
  elements.message.focus();
});
elements.message.addEventListener('input', () => {
  const context = composerContext();
  if (!context) return;
  const key = composerKey(context);
  const reply = replies.get(key);
  if (reply) reply.text = elements.message.value;
  else drafts.set(key, elements.message.value);
  fitMessage();
  notices.delete(key);
  const limit = composerLimit(reply);
  if (byteLength(elements.message.value) > limit) setNotice(context, `Messages can be up to ${limit.toLocaleString('en')} bytes.`, true);
  else renderComposer();
});
// Safari reports the composition-committing Enter with keyCode 229 and isComposing false.
elements.composer.addEventListener('keydown', (event) => {
  if (event.isComposing || event.keyCode === 229) return;
  if (event.key === 'Enter' && (event.metaKey || event.ctrlKey) && event.target === elements.message) {
    event.preventDefault();
    elements.send.click();
  } else if (event.key === 'Escape' && !elements['reply-context'].hidden) {
    event.preventDefault();
    event.stopPropagation();
    elements['cancel-reply'].click();
  }
});
byId('send-hint').textContent = `${sendKey} ↵`;
let composerHeight = 0;
function resizeComposerAndKeepTimelineAtBottom() {
  const height = elements['composer-container'].offsetHeight;
  const composerGrowth = height - composerHeight;
  composerHeight = height;
  const scroller = elements.conversation;
  const distanceFromPreviousBottom = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight - composerGrowth;
  if (composerGrowth > 0 && distanceFromPreviousBottom <= 4) scroller.scrollTop = scroller.scrollHeight;
  fitMessage();
}
new ResizeObserver(resizeComposerAndKeepTimelineAtBottom).observe(elements['composer-container']);
document.addEventListener('click', (event) => {
  if (menu && !event.target.closest('.row-menu, .menu-popover')) closeMenu(false);
});
document.addEventListener('keydown', (event) => {
  const digit = event.altKey && event.shiftKey && !event.ctrlKey && !event.metaKey && /^Digit([1-9])$/.exec(event.code);
  if (digit && !event.isComposing && !elements.detail.open &&
      !event.target.closest('input, textarea, select, [contenteditable]:not([contenteditable="false"])')) {
    const bot = pinnedBotsInShortcutOrder()[Number(digit[1]) - 1];
    if (bot) {
      event.preventDefault();
      selectBot(bot.id);
    }
    return;
  }
  if (event.key !== 'Escape') return;
  if (menu) closeMenu();
  else if (workspace.classList.contains('nav-open') && !elements.detail.open) openNavigation(false);
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
window.addEventListener('popstate', () => { if (listLoaded) selectLocation(); });
