import { accentColor, showAvatar } from '/avatars.js';
import { collectEventPages, mergeEventPages } from '/event-history.mjs';
import { createLiveUpdates } from '/live-updates.js';
import { createBotEditor } from '/bot-editor.js';

const byId = (id) => document.getElementById(id);
const elements = Object.fromEntries([
  'navigation', 'pinned', 'bot-list', 'wait-list', 'wait-count', 'session-list', 'more-sessions',
  'connection', 'connection-text', 'new-bot-button', 'open-navigation', 'close-navigation', 'profile', 'profile-avatar',
  'profile-name', 'profile-status', 'profile-placeholder', 'settings-button', 'conversation', 'older',
  'timeline', 'composer-container', 'composer', 'reply-context', 'reply-question', 'cancel-reply',
  'reply-label', 'reply-choice', 'compose-label', 'message', 'send', 'notice', 'detail', 'detail-title', 'detail-body', 'close-detail',
  'bot-editor', 'theme-switch', 'bot-activity', 'activity-scope-avatar', 'activity-scope-name',
  'peer-heading', 'peer-back', 'peer-picker', 'peer-status',
].map((id) => [id, byId(id)]));
const workspace = document.querySelector('.workspace');
const narrow = matchMedia('(max-width: 700px)');
const clock = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
const dayFormat = new Intl.DateTimeFormat('en', { year: 'numeric', month: 'long', day: 'numeric' });
const preciseClock = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23' });
const fullTime = new Intl.DateTimeFormat('en', { dateStyle: 'medium', timeStyle: 'short', hourCycle: 'h23' });
const byteLength = (text) => new TextEncoder().encode(text).length;
const stateLabels = {
  queued: 'Queued', deciding: 'Evaluating', sending: 'Sending', running: 'Working', waiting: 'Waiting for input',
  cancelling: 'Cancelling', stopping: 'Confirming stop', completed: 'Complete', cancelled: 'Cancelled', ended: 'Ended',
  failed: 'Failed', interrupted: 'Interrupted', rolled_back: 'Rolled back',
  idle: 'Idle', thinking: 'Thinking', working: 'Working', peer_waiting: 'Waiting for another Bot',
};
const waitLabels = {
  user_question: ['Question from the Bot', 'The Bot is waiting for your answer.'],
  budget: ['Approval for one more run', 'This request used all of its runs. Approving lets the Bot start one more run, which can use more model and T3 usage.'],
  model_budget: ['Approval for another model call', 'This request reached its model call limit. You can approve one more model call.'],
  model_failure: ['Retry decision', 'The Bot could not decide on the next step after several attempts. You can retry.'],
  t3_input: ['Action in T3', 'Open the session in T3 to approve or answer.'],
  deadline: ['Deadline extension', 'This request reached its deadline. You can set a new one.'],
  unknown_delivery: ['Delivery unconfirmed', 'Flock could not confirm whether T3 received the last message, so this work is paused. Reconcile checks T3 again without resending anything.'],
  auth: ['T3 sign-in needed', 'This work is paused because the T3 sign-in expired. Sign in to T3 again, then reconcile.'],
  settings: ['T3 settings', 'This work is paused because of the T3 connection or execution settings. Fix them, then reconcile.'],
};
const closedLabels = {
  answer: 'Answered', run_finished: 'Session ended', cancel: 'Stop requested',
  session_question: 'The session updated the question',
};
const errors = {
  invalid_text: 'Check the message and its length.', invalid_deadline: 'Set a deadline in the future.',
  intake_settings: 'Flock could not accept the request. Check the Bot’s execution settings.',
  message_conflict: 'A message was already recorded for this submission. Check the conversation.',
  operation_conflict: 'Flock already processed this action or the pending request changed.',
  invalid_candidate: 'That choice is no longer available. Check the latest confirmation.',
  bot_not_found: 'Bot not found.',
  invalid_peer: 'Choose another Bot for this DM.',
  peer_not_allowed: 'This Bot DM is unavailable.',
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
let view = null;
let menu = null;
let detail = null;
let profileAvatar = null;
let profileKey = '';
let scopeKey = '';
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
  const link = node('a', 'link', label);
  link.href = href;
  link.target = '_blank';
  link.rel = 'noopener noreferrer';
  return link;
}
function waitLabel(reason) { return waitLabels[reason]?.[0] ?? 'Waiting for input'; }
function waitText(wait) { return wait.text || waitLabels[wait.reason]?.[1] || 'Refresh the pending request.'; }
function startedText(seconds, precise = false) {
  return seconds ? `started ${(precise ? preciseClock : clock).format(seconds * 1000)}` : '';
}
function repeatedRequests(items) {
  const counts = new Map();
  for (const item of items) counts.set(item.request, (counts.get(item.request) ?? 0) + 1);
  return new Set([...counts].filter(([, count]) => count > 1).map(([request]) => request));
}
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
const timelineContext = () => view?.dm ?? view;
const showingComposer = (bot) => view?.id === bot && !view?.dm;
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
    sessionsBefore: 0, ...existing, loadingOlder: false, loadingSessions: false,
    dm: peerId ? { id: botId, peerId, events: new Map(), olderBefore: 0 } : null } : null;
  const pending = botId && pendingMessage(botId);
  if (pending?.reply && !replies.has(botId)) replies.set(botId, pending.reply);
  else if (pending && !pending.reply && !drafts.has(botId)) drafts.set(botId, pending.payload.text);
  const url = conversationUrl(botId, peerId);
  if (url.href !== location.href) {
    if (updateHistory) history.pushState(null, '', url);
    else history.replaceState(null, '', url);
  }
  save('flock-selected-bot', botId);
  elements.conversation.scrollTop = 0;
  render('bottom');
  if (view) void requestSync({ bot: true, dm: !!view.dm, scroll: 'bottom' });
  subscribeToView();
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

let botDraft = null;
function sidebarBots() {
  if (botDraft?.mode !== 'edit') return bots;
  return bots.map((bot) => (bot.id === botDraft.botId ? { ...bot, name: botDraft.name, avatar: botDraft.avatar } : bot));
}
function pinnedBotsInShortcutOrder(source = bots) {
  return [...pins].map((id) => source.find((bot) => bot.id === id)).filter(Boolean);
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

function draftRow(draft) {
  const row = node('div', 'bot-row draft');
  row.setAttribute('aria-hidden', 'true');
  const select = node('span', 'bot-select');
  const name = node('span', 'bot-name');
  name.append(node('span', 'bot-name-text', draft.name));
  const text = node('span', 'bot-text');
  text.append(name, node('span', 'bot-preview', 'No conversation yet'));
  select.append(avatar(draft, 'bot-avatar'), text);
  row.append(select);
  return row;
}
function renderBots() {
  if (!changed('bots', bots, [...pins], menu, view?.id, listLoaded, botDraft)) return;
  const focusedSidebarKey = sidebar.contains(document.activeElement) ? document.activeElement.dataset.focus : null;
  const source = sidebarBots();
  const pinned = pinnedBotsInShortcutOrder(source);
  const presenceRank = (bot) => (activePresence.has(bot.presence) ? 0 : 1);
  const unpinnedByPresence = source.filter((bot) => !pins.has(bot.id)).sort((a, b) => presenceRank(a) - presenceRank(b));
  elements.pinned.replaceChildren(...pinned.map(pinTile));
  elements.pinned.hidden = !pinned.length;
  const list = elements['bot-list'];
  list.setAttribute('aria-busy', !listLoaded);
  list.replaceChildren(...unpinnedByPresence.map(botRow));
  if (botDraft?.mode === 'create') list.prepend(draftRow(botDraft));
  if (!listLoaded) list.append(...skeletonRows(4, 'skeleton-bot'));
  else if (!bots.length && !botDraft) list.append(node('p', 'activity-empty', 'No Bots configured'));
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
  elements['bot-activity'].hidden = !bot || !!dm;
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
  const nextScopeKey = bot ? JSON.stringify([bot.id, bot.name, bot.avatar]) : '';
  if (bot && nextScopeKey !== scopeKey) {
    showAvatar(elements['activity-scope-avatar'], bot);
    elements['activity-scope-name'].textContent = bot.name;
  }
  scopeKey = nextScopeKey;
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
  } else if (profile.dataset.presence !== presence) profileAvatar?.update({ state: avatarState });
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
  row.title = [task.request, task.createdAt ? `Started ${fullTime.format(task.createdAt * 1000)}` : ''].filter(Boolean).join(' · ');
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
    elements['wait-count'].hidden = !(waits.length + clarifications.length);
    elements['wait-list'].setAttribute('aria-busy', loading);
    const repeated = repeatedRequests(waits);
    const rows = waits.map((task) => {
      const status = [waitLabel(task.wait.reason), repeated.has(task.request) ? startedText(task.createdAt) : '']
        .filter(Boolean).join(' · ');
      const row = activityRow('button', 'wait-row', task.request, status, 'waiting');
      row.type = 'button';
      row.title = waitText(task.wait);
      row.addEventListener('click', () => {
        openNavigation(false);
        openDetail({ kind: 'wait', taskId: task.id, waitId: task.wait.id });
      });
      return row;
    });
    for (const clarification of clarifications) {
      const row = activityRow('button', 'wait-row', clarification.text, 'Clarification requested', 'waiting');
      row.type = 'button';
      row.title = clarification.text;
      row.addEventListener('click', () => {
        openNavigation(false);
        openDetail({ kind: 'clarification', messageId: clarification.messageId });
      });
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
    live.push(sessionRow({ request: run.request, createdAt: run.createdAt }, run, stateLabels[run.state] ?? run.state, true));
  }
  const ended = [];
  for (const task of [...groups.values()].sort((a, b) => b.id - a.id)) {
    for (const session of task.sessions) {
      if (!activeKeys.has(key(task.id, session))) ended.push(sessionRow(task, session, 'Ended', false));
    }
  }
  const rows = [];
  const grouped = live.length > 0 && ended.length > 0;
  for (const [label, items] of [['Active', live], ['Ended', ended]]) {
    if (!items.length) continue;
    if (!grouped) { rows.push(...items); continue; }
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

function beginReply(target, text = '', choice = null) {
  if (!target.messageId || sendingMessages.has(view.id)) return;
  if (view.dm) selectBot(view.id);
  const current = replies.get(view.id);
  if (current?.closed && current.text.trim()) drafts.set(view.id, current.text);
  if (current?.messageId !== target.messageId || text || choice || current?.candidateTaskId)
    replies.set(view.id, { ...target, text, ...(choice ? { candidateTaskId: choice.taskId, choice: choice.label } : {}) });
  closeDetail();
  renderComposer();
  elements.message.focus();
}
const icons = {
  copy: '<path d="M9 9.5A1.5 1.5 0 0 1 10.5 8h7A1.5 1.5 0 0 1 19 9.5v9a1.5 1.5 0 0 1-1.5 1.5h-7A1.5 1.5 0 0 1 9 18.5z"/><path d="M15 8V5.5A1.5 1.5 0 0 0 13.5 4h-7A1.5 1.5 0 0 0 5 5.5v9A1.5 1.5 0 0 0 6.5 16H9"/>',
  copied: '<path d="m5.5 12.5 4 4 9-9"/>',
  reply: '<path d="M9.5 7 4.5 12l5 5"/><path d="M5 12h9a5.5 5.5 0 0 1 5.5 5.5V19"/>',
  forward: '<path d="m14.5 7 5 5-5 5"/><path d="M19 12h-9a5.5 5.5 0 0 0-5.5 5.5V19"/>',
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
function candidateButton(target, candidate, order) {
  const started = startedText(candidate.createdAt, !!order);
  const state = candidate.state ? stateLabels[candidate.state] ?? '' : '';
  const label = order ? `${candidate.request} (${[order, started].filter(Boolean).join(', ')})` : candidate.request;
  const option = button(null, 'button candidate', () => beginReply(target, label, { taskId: candidate.taskId, label }));
  option.append(node('span', 'candidate-request', candidate.request));
  const detail = [order, started, state].filter(Boolean).join(' · ');
  if (detail) option.append(node('span', 'candidate-detail', detail));
  option.setAttribute('aria-label', [candidate.request, detail].filter(Boolean).join(', '));
  option.disabled = candidate.state === 'ended';
  return option;
}
function clarificationTitle(clarification) {
  return clarification?.route === 'model_budget' ? waitLabel('model_budget') : 'Clarification requested';
}
function clarificationActions(clarification) {
  const actions = node('div', 'event-actions');
  if (clarification.route === 'model_budget') {
    const waitMessageId = clarification.messageId;
    const key = `message/${waitMessageId}`;
    const approve = button('Approve one more model call', 'button',
      () => void submitOperation(view, key, { waitMessageId, operation: 'approve' }));
    approve.disabled = pendingWaits.has(key);
    actions.append(approve);
    return actions;
  }
  const target = { kind: 'clarification', messageId: clarification.messageId, summary: clarification.text };
  if (clarification.destinations) {
    for (const destination of clarification.destinations)
      actions.append(button(destination, 'button', () => beginReply(target, destination)));
  } else {
    const candidates = clarification.candidates ?? [];
    const repeated = repeatedRequests(candidates);
    const ordinals = ['first', 'second', 'third', 'fourth', 'fifth', 'sixth', 'seventh', 'eighth', 'ninth', 'tenth'];
    const order = (candidate) => {
      if (!repeated.has(candidate.request)) return '';
      const same = candidates.filter((item) => item.request === candidate.request)
        .sort((a, b) => (a.createdAt ?? 0) - (b.createdAt ?? 0) || a.taskId - b.taskId);
      const index = same.indexOf(candidate);
      return index === same.length - 1 ? 'requested last' : `requested ${ordinals[index] ?? `${index + 1}th`}`;
    };
    for (const candidate of candidates) actions.append(candidateButton(target, candidate, order(candidate)));
  }
  actions.append(button('Write a reply', 'text-button', () => beginReply(target)));
  return actions;
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
function sourceAction(source, label = 'Open source') {
  return button(label, 'text-button', async () => {
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
function runIcon(run) {
  const link = linkToRun(run);
  if (!link) return null;
  link.className = 'run-link';
  link.title = 'Open in T3';
  link.setAttribute('aria-label', 'Open in T3');
  link.replaceChildren(icon('open'));
  return link;
}
function eventCard(event, context, place) {
  const data = event.data;
  const bot = context.bot;
  const side = messageSide(event);
  if (side === 'system') {
    const stopped = data.request ? `“${data.request.split('\n')[0]}” was stopped` : 'A request was stopped';
    const note = node('article', 'system-note', `${bot?.name ?? 'Bot'} · ${stopped} · `);
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
  meta.append(name, eventTime(event, scheduled));
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
  let replyTarget = null;
  let copy = data.text ?? '';
  const pendingWait = event.kind === 'waiting' ? context.waits.get(data.id) : null;

  if (event.kind === 'waiting') {
    copy = waitText(data);
    text.textContent = copy;
    bubble.append(text);
    if (data.decisionReason) bubble.append(reasonText(data.decisionReason));
    if (pendingWait?.wait.reason === 'user_question') extras.push(questionActions(view, pendingWait));
    else if (pendingWait) {
      const open = node('div', 'event-actions question-actions');
      open.append(button('View pending request', 'button', () => openDetail({ kind: 'wait', taskId: pendingWait.id, waitId: data.id })));
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
      extras.push(links);
      if (data.route === 'respond') replyTarget = ['Reply to this answer', { kind: 'result', messageId: event.id, summary: data.text }];
    }
  } else if (event.kind === 'report') {
    copy = data.report || 'The session submitted a report.';
    text.textContent = copy;
    const link = runIcon(data.run);
    if (link) text.append(link);
    bubble.append(text);
    if (data.report) replyTarget = ['Reply to this result', { kind: 'result', messageId: event.id, summary: data.report }];
    if (data.status && data.status !== 'completed') {
      bubble.append(node('p', 'event-reason', `Run status: ${stateLabels[data.status] ?? data.status}`));
    }
    if (data.truncated) bubble.append(node('p', 'event-reason', 'Open T3 to read the full report.'));
  } else if (event.kind === 'completed') {
    bubble.classList.add('completed');
    text.append(icon('check', 'glyph kind-icon'), node('span', 'sr-only', 'Complete: '), document.createTextNode(data.text ?? ''));
    const link = runIcon(data.run);
    if (link) text.append(link);
    bubble.append(text);
    if (data.text) replyTarget = ['Reply to this result', { kind: 'result', messageId: event.id, summary: data.text }];
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
      const note = node('p', 'msg-note', `Waiting for “${blocker.request}” (${owner}) to finish`);
      note.title = blocker.request;
      extras.push(note);
    }
  }
  if (copy) {
    const control = iconButton('copy', 'Copy text', () => void copyMessage(control, copy));
    actions.append(control);
  }
  if (pendingWait?.wait.reason === 'user_question') {
    const reply = iconButton('reply', 'Write a reply', () => beginReply({ kind: 'wait', messageId: pendingWait.wait.messageId, summary: waitText(pendingWait.wait) }));
    reply.disabled = pendingWaits.has(pendingWait.wait.id);
    actions.append(reply);
  }

  if (replyTarget) {
    const [label, target] = replyTarget;
    actions.append(iconButton('reply', label, () => beginReply(target)));
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

function peerExchangeLink(messages, activeBot) {
  const first = messages[0];
  const { fromBotId, toBotId } = first.data;
  const peer = participant(fromBotId === activeBot ? toBotId : fromBotId);
  const count = messages.length === 1 ? '1 message' : `${messages.length} messages`;
  const link = peerLink(activeBot, peer.id, null, 'peer-exchange-link');
  link.append(avatar(peer, 'peer-exchange-avatar'), node('span', null, `${count} with ${peer.name}`));
  link.dataset.peerFocus = first.id;
  return link;
}

function peerThread(events, dm) {
  const fragment = document.createDocumentFragment();
  if (!events.length) {
    const empty = node('div', 'empty-state');
    const title = dm.error ? 'DM unavailable' : dm.loaded ? 'No messages yet' : 'Loading…';
    empty.append(node('h2', null, title));
    if (dm.error) empty.append(node('p', null, dm.error));
    fragment.append(empty);
  }
  let day = '';
  for (const event of events) {
    const nextDay = dayLabel(event.time);
    if (nextDay !== day) {
      fragment.append(node('p', 'day', nextDay));
      day = nextDay;
    }
    const { fromBotId, text, forwardedMessage } = event.data;
    const from = participant(fromBotId);
    const row = node('article', 'msg group-start peer-dm-message');
    if (forwardedMessage) {
      const content = node('div', 'peer-forward-content');
      const label = node('div', 'peer-forward-meta');
      label.append(icon('forward'), node('span', null, `Forwarded by ${from.name}`), eventTime(forwardedMessage));
      const card = node('div', 'bubble peer-forwarded-message');
      card.append(node('p', 'event-text', forwardedMessage.text));
      content.append(label, card);
      row.append(content);
    }
    if (text || !forwardedMessage) {
      const metadata = node('div', 'msg-meta');
      metadata.append(node('span', 'msg-name', from.name), eventTime({ time: event.time }));
      const line = node('div', 'msg-line');
      const bubble = node('div', 'bubble');
      bubble.append(node('p', 'event-text', text));
      line.append(avatar(from, 'msg-avatar'), bubble);
      row.append(metadata, line);
    }
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
  const context = timelineContext();
  const events = [...(context?.events.values() ?? [])].sort((a, b) => a.id - b.id);
  older.hidden = !context?.olderBefore;
  older.disabled = !!context?.loadingOlder;
  older.textContent = context?.loadingOlder ? 'Loading…' : 'Show earlier conversation';
  const peerStatus = elements['peer-status'];
  const olderFailed = view?.dm?.olderFailed;
  peerStatus.hidden = !olderFailed;
  peerStatus.textContent = olderFailed ? 'Could not load earlier messages.' : '';
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
    if (!changed('timeline', dm.id, dm.peerId, events, dm.loaded, dm.error, participants, dayFormat.format(Date.now()))) return;
    scroller.setAttribute('aria-label', 'Bot DM history');
    elements.timeline.replaceChildren(peerThread(events, dm));
    moveScroll();
    return;
  }
  if (!changed('timeline', view?.id, data?.bot, events, data?.waits, data?.queued, [...pendingWaits], listLoaded,
    data?.presence, data?.activeSessions, data?.sessionTasks, data?.clarifications, bots, dayFormat.format(Date.now()))) return;
  scroller.setAttribute('aria-label', 'Conversation history');
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
    fragment.append(peerExchangeLink(peerMessages, view.id));
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
    else if (loaded && !bots.length) {
      empty.append(node('p', null, 'Create a Bot to get started.'));
      empty.append(button('New bot', 'dialog-button primary', () => openNewBot(elements['new-bot-button'])));
    }
    fragment.append(empty);
  }
  if (data && (data.presence === 'working' || data.presence === 'thinking')) fragment.append(workingLine(data));
  elements.timeline.replaceChildren(fragment);
  moveScroll();
}

const sendKey = /mac|iphone|ipad/i.test(navigator.userAgentData?.platform ?? navigator.platform) ? '⌘' : 'Ctrl';
function setNotice(context, text, isError = false) {
  notices.set(context.id, { text, isError });
  if (showingComposer(context.id)) renderComposer();
}
const replyLabels = { wait: 'Reply to a question', clarification: 'Reply to a clarification', result: 'Reply to a result' };
const composerLimit = (reply) => reply?.kind === 'wait' ? 8000 : 16000;
function scheduleNoticeFade(context, notice) {
  const bot = context.id;
  if (!notice || notice.isError || notice.timer || sendingMessages.has(bot) || [...pendingWaits.values()].includes(bot)) return;
  notice.timer = setTimeout(() => {
    notice.timer = null;
    if (notices.get(bot) !== notice || !showingComposer(bot)) return;
    notice.fading = true;
    renderComposer();
    setTimeout(() => {
      if (notices.get(bot) !== notice) return;
      notices.delete(bot);
      if (showingComposer(bot)) renderComposer();
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
  const context = view;
  const available = context?.data && !context.dm;
  elements['composer-container'].hidden = !available;
  if (!available) return;
  const reply = replies.get(context.id);
  const notice = notices.get(context.id);
  const body = reply ? reply.text : drafts.get(context.id) ?? '';
  const busy = sendingMessages.has(context.id);
  const closedReply = reply?.closed === true;
  elements['reply-context'].hidden = !reply;
  elements['reply-label'].textContent = reply ? replyLabels[reply.kind] : '';
  elements['reply-question'].textContent = reply?.summary ?? '';
  elements['reply-choice'].hidden = !reply?.choice;
  elements['reply-choice'].textContent = reply?.choice ? `Selected: ${reply.choice}` : '';
  elements['compose-label'].textContent = reply ? replyLabels[reply.kind] : 'Message';
  elements['cancel-reply'].disabled = busy;
  elements.message.placeholder = reply ? 'Write a reply' : `Message ${context.data.bot.name}`;
  if (elements.message.value !== body) {
    elements.message.value = body;
    fitMessage();
  }
  elements.message.readOnly = busy || closedReply;
  elements.composer.setAttribute('aria-busy', String(busy));
  elements.send.disabled = busy || closedReply || !body.trim() || byteLength(body) > composerLimit(reply);
  elements.composer.classList.toggle('can-send', !elements.send.disabled);
  elements.send.setAttribute('aria-label', reply ? 'Send reply' : 'Send message');
  elements.send.title = `${reply ? 'Reply' : 'Send'} (${sendKey}+Enter)`;
  elements.notice.textContent = notice?.isError ? notice.text :
    (closedReply ? 'This question is closed. You can still copy your reply.' : notice?.text ?? '');
  elements.notice.classList.toggle('error', !!notice?.isError);
  elements.notice.classList.toggle('fading', !closedReply && !!notice?.fading);
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
  if (!view?.data) return;
  detail = selection.kind === 'settings' ? { section: 'general', ...selection } : selection;
  signatures.delete('detail');
  renderDetail();
  if (elements.detail.open) return;
  detailOrigin = document.activeElement;
  elements.detail.showModal();
  if (!detailNav.hidden) detailNav.querySelector('[aria-selected="true"]')?.focus();
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
let reopenSettingsOnClose = false;
const botEditor = createBotEditor(elements['bot-editor'], {
  api,
  onDraft(draft) {
    botDraft = draft;
    renderBots();
  },
  async onSaved(savedBot, savedMode) {
    const returnToSettings = reopenSettingsOnClose;
    reopenSettingsOnClose = false;
    await requestSync({ list: true, bot: view?.id === savedBot.id });
    if (savedMode === 'create') {
      selectBot(savedBot.id);
      return;
    }
    if (returnToSettings && view?.id === savedBot.id && view.data) {
      openDetail({ kind: 'settings', section: 'general', status: 'Appearance updated' });
    }
  },
  onCancel(savedMode) {
    const returnToSettings = reopenSettingsOnClose;
    reopenSettingsOnClose = false;
    if (savedMode === 'edit' && returnToSettings && view?.data) {
      openDetail({ kind: 'settings', section: 'general' });
    }
  },
});

function openNewBot(origin = elements['new-bot-button']) {
  reopenSettingsOnClose = false;
  closeMenu(false);
  openNavigation(false, false);
  closeDetail();
  botEditor.openCreate({ bots, origin });
}

function openEditAppearance(bot) {
  reopenSettingsOnClose = true;
  closeDetail();
  botEditor.openEdit(bot, { bots, origin: elements['settings-button'] });
}

function generalContent(bot) {
  const presence = view.data.presence ?? 'idle';
  const hero = node('div', 'settings-hero');
  const face = node('span', 'settings-avatar');
  detailAvatar = { state: presence, mount: () => {
    return showAvatar(face, bot, { still: false, state: presence });
  } };
  hero.append(
    face,
    node('p', 'settings-name', bot.name),
    button('Edit appearance', 'dialog-button quiet settings-edit-button', () => openEditAppearance(bot)),
  );
  if (detail?.status) {
    const note = node('p', 'settings-status', detail.status);
    note.setAttribute('role', 'status');
    hero.append(note);
  }
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
  request.append(node('p', 'wait-request-text', task.request));
  if (task.createdAt) request.append(node('p', 'wait-request-meta', `Started ${fullTime.format(task.createdAt * 1000)}`));
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
  else if (wait.reason === 'model_budget') primary.append(action('Approve one more model call', 'approve'));
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
  }
  actions.append(button('Stop this request', 'wait-cancel', () => {
    const name = task.request.split('\n')[0];
    if (window.confirm(`Stop “${name}”?\n\nThe Bot will stop working on this request and interrupt its T3 session if one is running.`))
      perform('cancel');
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
    const peers = botPeers(view.id).map(({ id }) => participant(id));
    if (changed('detail', detail, peers.map(({ id, name, avatar }) => [id, name, avatar]))) {
      const active = document.activeElement?.dataset.peerChoice;
      elements['detail-title'].textContent = 'Bot DMs';
      const list = node('ul', 'peer-picker-list');
      for (const bot of peers) {
        const item = node('li');
        const link = peerLink(view.id, bot.id, null, 'peer-choice');
        link.dataset.peerChoice = bot.id;
        link.append(avatar(bot, 'peer-choice-avatar'), node('span', 'peer-choice-name', bot.name));
        item.append(link);
        list.append(item);
      }
      replaceDetail(() => peers.length ? [list] : [node('p', 'prose', 'No Bot DMs available.')]);
      if (active) [...list.querySelectorAll('a')].find((link) => link.dataset.peerChoice === active)?.focus();
    }
    return;
  }
  if (detail.kind === 'source') {
    if (changed('detail', detail)) {
      elements['detail-title'].textContent = 'Original message';
      const source = detail.event;
      const text = source?.kind === 'waiting' ? waitText(source.data) :
        source?.data.report ?? source?.data.text ??
        (detail.failed ? 'The original message is no longer available in this conversation.' : 'Loading…');
      const body = node('p', 'prose', text);
      elements['detail-body'].replaceChildren(body);
      const link = linkToRun(source?.data.run);
      if (link) {
        const actions = node('div', 'event-actions');
        actions.append(link);
        elements['detail-body'].append(actions);
      }
    }
    return;
  }
  if (detail.kind === 'clarification') {
    const clarification = view.data.clarifications.find((item) => item.messageId === detail.messageId);
    if (changed('detail', detail, clarification, [...pendingWaits])) {
      elements['detail-title'].textContent = clarificationTitle(clarification);
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
  if (!context.loadedSessions.size) context.sessionsBefore = latest.sessionsBefore;
  context.data = latest;
  const reply = replies.get(context.id);
  if (!reply || sendingMessages.has(context.id)) return;
  if (reply.kind === 'wait' && !latest.waits.some((task) => task.wait.messageId === reply.messageId)) {
    if (reply.text.trim()) reply.closed = true;
    else {
      replies.delete(context.id);
      setNotice(context, 'That question is no longer open, so your reply was not sent.');
    }
  } else if (reply.kind === 'clarification' &&
             !latest.clarifications.some((item) => item.messageId === reply.messageId)) {
    if (reply.text.trim()) reply.closed = true;
    else {
      replies.delete(context.id);
      setNotice(context, 'That confirmation has closed, so your reply was not sent.');
    }
  }
}
async function conversationPage(timeline, before) {
  if (!timeline.peerId) return api(botQuery('conversation', timeline.id, before));
  const { messages, hasOlder } = await api(peerQuery(timeline, before));
  return { events: messages, olderBefore: hasOlder ? messages[0].id : 0 };
}
async function refreshPeerConversation(context) {
  const dm = context.dm;
  const isCurrent = () => context === view;
  const readPage = before => conversationPage(dm, before);
  try {
    const latest = await readPage();
    const pages = await collectEventPages(latest, dm.newestEvent, readPage, isCurrent);
    if (!pages || !isCurrent()) return true;
    mergeEventPages(dm, pages);
    dm.loaded = true;
    dm.error = null;
  } catch (error) {
    if (!isCurrent()) return true;
    if (!dm.loaded) dm.error = errors[error.message] ?? 'Could not load this conversation.';
    return [403, 404].includes(error.status);
  }
  return true;
}
async function refreshView(context) {
  const isCurrent = () => context === view;
  try {
    const latest = await api(botQuery('view', context.id));
    const pages = await collectEventPages(latest, context.newestEvent, before => conversationPage(context, before), isCurrent);
    if (!pages || !isCurrent()) return true;
    mergeEventPages(context, pages);
    mergeLatest(context, latest);
    setConnection('ready');
    return true;
  } catch (error) {
    if (!isCurrent()) return true;
    if (error.status === 404) {
      void requestSync({ list: true });
      return true;
    }
    setConnection('offline');
    return false;
  }
}
async function refreshBots() {
  try {
    const latest = await api('bots');
    bots = latest;
    listLoaded = true;
    setConnection('ready');
    if (!bots.some((bot) => bot.id === view?.id)) selectLocation();
    return true;
  } catch {
    setConnection('offline');
    return false;
  }
}
async function loadOlder() {
  const context = timelineContext();
  const cursor = context?.olderBefore;
  if (!cursor || context.loadingOlder) return;
  context.loadingOlder = true;
  if (context.peerId) context.olderFailed = false;
  renderTimeline();
  try {
    const { events, olderBefore } = await conversationPage(context, cursor);
    if (context !== timelineContext() || context.olderBefore !== cursor) return;
    context.olderBefore = olderBefore;
    for (const event of events) context.events.set(event.id, event);
    renderTimeline('older');
  } catch {
    if (context !== timelineContext()) return;
    if (context.peerId) context.olderFailed = true;
    else setNotice(context, 'Could not load earlier messages.', true);
  }
  finally {
    context.loadingOlder = false;
    if (context === timelineContext()) renderTimeline();
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
  await submitOperation(context, waitId, { taskId: task.id, waitId, operation, ...fields });
}
async function submitOperation(context, key, payload) {
  if (!context || pendingWaits.has(key)) return;
  const isOpen = () => view === context && (payload.waitMessageId
    ? detail?.kind === 'clarification' && detail.messageId === payload.waitMessageId
    : detail?.waitId === payload.waitId);
  pendingWaits.set(key, context.id);
  setNotice(context, 'Submitting action…');
  if (view?.id === context.id) render();
  let failed = false;
  try {
    await api('operations', { botId: context.id, ...payload });
    setNotice(context, payload.operation === 'answer' ? 'Answer received.' : 'Action received.');
    if (isOpen()) closeDetail();
  } catch (error) {
    const message = errors[error.message] ?? 'Flock could not confirm the action. Check the conversation.';
    setNotice(context, message, true);
    if (isOpen()) showDetailError(message);
    failed = true;
  } finally {
    if ((failed || !liveUpdates.live) && view?.id === context.id) await requestSync({ bot: true, list: true });
    pendingWaits.delete(key);
    if (view?.id === context.id) render();
  }
}
async function submit(event) {
  event.preventDefault();
  const context = view;
  if (!context?.data || context.dm || elements.send.disabled) return;
  const reply = replies.get(context.id);
  const text = reply ? reply.text : drafts.get(context.id);
  const replyToMessageId = reply?.messageId;
  const candidateTaskId = reply?.candidateTaskId;
  const previous = pendingMessage(context.id)?.payload;
  const clientMessageId = previous?.text === text && previous.replyToMessageId === replyToMessageId &&
    previous.candidateTaskId === candidateTaskId ? previous.clientMessageId : crypto.randomUUID();
  const payload = { botId: context.id, clientMessageId, text };
  if (replyToMessageId) payload.replyToMessageId = replyToMessageId;
  if (replyToMessageId && candidateTaskId) payload.candidateTaskId = candidateTaskId;
  rememberMessage(context.id, { payload, reply });
  sendingMessages.add(context.id);
  setNotice(context, 'Sending message…');
  try {
    await api('messages', payload);
    rememberMessage(context.id, null);
    if (reply) replies.delete(context.id);
    else drafts.delete(context.id);
    setNotice(context, 'Message received.');
    if (showingComposer(context.id)) elements.conversation.scrollTop = elements.conversation.scrollHeight;
    if (!liveUpdates.live) await requestSync({ list: true, bot: showingComposer(context.id), scroll: 'bottom' });
  } catch (error) {
    if (error.status >= 400 && error.status < 500) rememberMessage(context.id, null);
    setNotice(context, errors[error.message] ?? 'Flock could not confirm receipt. You can resend the same message.', true);
  }
  finally {
    sendingMessages.delete(context.id);
    if (showingComposer(context.id)) renderComposer();
  }
}

const savedTheme = saved('flock-theme', 'system');
renderTheme(['light', 'dark'].includes(savedTheme) ? savedTheme : 'system');
elements['theme-switch'].addEventListener('click', (event) => {
  const option = event.target.closest('button[data-theme-mode]');
  if (option) setTheme(option.dataset.themeMode);
});
elements['new-bot-button'].addEventListener('click', () => openNewBot(elements['new-bot-button']));
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
  replies.delete(view.id);
  renderComposer();
  elements.message.focus();
});
elements.message.addEventListener('input', () => {
  if (!view || view.dm) return;
  const reply = replies.get(view.id);
  if (reply) reply.text = elements.message.value;
  else drafts.set(view.id, elements.message.value);
  if (reply?.candidateTaskId && reply.text !== reply.choice) {
    delete reply.candidateTaskId;
    delete reply.choice;
  }
  fitMessage();
  notices.delete(view.id);
  const limit = composerLimit(reply);
  if (byteLength(elements.message.value) > limit) setNotice(view, `Messages can be up to ${limit.toLocaleString('en')} bytes.`, true);
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
  if (digit && !event.isComposing && !elements.detail.open && !elements['bot-editor'].open &&
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
  else if (workspace.classList.contains('nav-open') && !elements.detail.open && !elements['bot-editor'].open) openNavigation(false);
});

let pendingSync;
let syncInFlight;
let syncRetry;

function requestSync({ list = false, bot = false, dm = false, scroll } = {}) {
  if (!pendingSync || pendingSync.context !== view) {
    pendingSync = { list: pendingSync?.list, context: view };
  }
  pendingSync.list ||= list;
  pendingSync.bot ||= bot;
  pendingSync.dm ||= dm;
  pendingSync.scroll ??= scroll;
  syncInFlight ??= Promise.resolve().then(async () => {
    try {
      while (pendingSync) {
        const next = pendingSync;
        pendingSync = null;
        await syncView(next);
      }
    } finally {
      syncInFlight = null;
    }
  });
  return syncInFlight;
}

async function syncView({ context, list, bot, dm, scroll }) {
  const selected = context && context === view;
  const results = await Promise.all([
    !list || refreshBots(),
    !bot || !selected || refreshView(context),
    !dm || !selected || !context.dm || refreshPeerConversation(context),
  ]);
  render(view === context ? scroll : undefined);
  if (bot && view === context && context?.data && context.openSettingsOnLoad) {
    const botId = context.openSettingsOnLoad;
    context.openSettingsOnLoad = null;
    openSettingsFromMenu(botId);
  }
  if (results.includes(false) && !document.hidden) {
    syncRetry ??= setTimeout(syncAll, 2500);
  }
}

function syncAll() {
  clearTimeout(syncRetry);
  syncRetry = null;
  if (!document.hidden) void requestSync({ list: true, bot: !!view, dm: !!view?.dm });
}

const liveUpdates = createLiveUpdates(syncAll, (change) => {
  const dm = view?.dm;
  void requestSync({
    list: change.list === true,
    bot: !!view && change.bot === view.id,
    dm: !!dm && change.dm?.bot === dm.id && change.dm.peer === dm.peerId,
  });
});

function subscribeToView() {
  if (document.hidden) {
    liveUpdates.use(null);
    return;
  }
  const params = new URLSearchParams();
  if (view) params.set('bot', view.id);
  if (view?.dm) params.set('peer', view.dm.peerId);
  liveUpdates.use(params.size ? `/api/events?${params}` : '/api/events');
}

function resumeUpdates() {
  subscribeToView();
  syncAll();
}

render();
resumeUpdates();
document.addEventListener('visibilitychange', resumeUpdates);
window.addEventListener('pagehide', () => liveUpdates.use(null));
window.addEventListener('pageshow', (event) => { if (event.persisted) resumeUpdates(); });
window.addEventListener('popstate', () => { if (listLoaded) selectLocation(); });
