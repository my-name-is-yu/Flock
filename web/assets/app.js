import { handleComposerEnter } from '/composer-shortcuts.mjs';
import { accentColor, presenceMotion, showAvatar } from '/avatars.js';
import { refreshEventHistory, timelineMessages } from '/event-history.mjs';
import { coalescedRefresh, createLiveUpdates } from '/live-updates.js';
import { createBotEditor } from '/bot-editor.js';
import { mountBoardInbox } from '/board-inbox.js';
import { ScreenshotError, screenshotMessage, stageScreenshots } from '/composer-screenshots.mjs';
import { createTimelineRows, placeRows } from '/timeline-rows.js';
import { replyArrivalId } from '/reply-arrival.mjs';
import { questionChoiceForm, updateQuestionChoiceForm } from '/question-choices.js';
import { createComposerDraftStore } from '/composer-drafts.mjs';

const byId = (id) => document.getElementById(id);
const elements = Object.fromEntries([
  'navigation', 'pinned', 'bot-list', 'wait-list', 'wait-count', 'session-list', 'more-sessions',
  'connection', 'connection-text', 'new-bot-button', 'open-navigation', 'close-navigation', 'profile', 'profile-avatar',
  'profile-name', 'profile-status', 'profile-heading', 'profile-placeholder', 'conversation', 'older',
  'timeline', 'jump-to-latest', 'composer-container', 'composer', 'reply-context', 'reply-question', 'cancel-reply',
  'reply-label', 'compose-label', 'message', 'send', 'notice', 'view-only-notice', 'detail', 'detail-title', 'detail-body', 'close-detail',
  'attach-screenshots', 'screenshot-input', 'screenshot-previews',
  'bot-editor', 'theme-switch', 'bot-activity', 'activity-scope-avatar', 'activity-scope-name',
  'peer-identity', 'peer-heading', 'peer-faces', 'peer-back', 'peer-back-label', 'peer-picker', 'peer-status',
].map((id) => [id, byId(id)]));
const updateTimelineRows = createTimelineRows(elements.timeline, elements.conversation);
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
function animateLiveReply(id) {
  byId(`event-${id}`)?.querySelector('.bubble')?.animate?.([
    { opacity: .76, transform: 'translateY(4px)' },
    { opacity: 1, transform: 'none' },
  ], { duration: 125, easing: 'cubic-bezier(0.23, 1, 0.32, 1)' });
}
const workspace = document.querySelector('.workspace');
const narrow = matchMedia('(max-width: 700px)');
const clock = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
const dayFormat = new Intl.DateTimeFormat('en', { year: 'numeric', month: 'long', day: 'numeric' });
const preciseClock = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23' });
const fullTime = new Intl.DateTimeFormat('en', { dateStyle: 'medium', timeStyle: 'short', hourCycle: 'h23' });
const byteLength = (text) => new TextEncoder().encode(text).length;
const sessionEndLabels = {
  completed: 'Complete', cancelled: 'Cancelled', failed: 'Failed',
  interrupted: 'Interrupted', rolled_back: 'Rolled back', ended: 'Ended',
};
const stateLabels = {
  ...sessionEndLabels,
  queued: 'Queued', deciding: 'Evaluating', sending: 'Sending', running: 'Working', waiting: 'Waiting for input',
  cancelling: 'Cancelling', stopping: 'Confirming stop',
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
  session_question: 'The session updated the question', withdrawn: 'Question withdrawn', request_changed: 'Request changed',
};
const errors = {
  invalid_text: 'Check the message and its length.', invalid_deadline: 'Set a deadline in the future.',
  intake_settings: 'Flock could not accept the request. Check the Bot’s execution settings.',
  message_conflict: 'A message was already recorded for this submission. Check the conversation.',
  operation_conflict: 'Flock already processed this action or the pending request changed.',
  request_changed: 'The request changed. Review its current details before acting again.',
  already_closed: 'This request was already decided or closed.',
  access_denied: 'You do not have permission to make this decision.',
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
const composerDrafts = createComposerDraftStore();
const screenshotDrafts = new WeakMap();
const sendingMessages = new Set();
const pendingWaits = new Map();
const notices = new Map();
const signatures = new Map();
const readingPositions = new Map();
let bots = [];
let listLoaded = false;
let view = null;
let visibilityGeneration = 0;
let menu = null;
let detail = null;
let profileAvatar = null;
let profileKey = '';
let profileMotion;
let scopeAvatar = null;
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
const responseViewers = new WeakMap();
async function api(path, body) {
  const init = { credentials: 'same-origin' };
  if (body) Object.assign(init, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Flock-Action': '1' },
    body: JSON.stringify(body),
  });
  const response = await fetch(`/api/${path}`, init);
  const data = await response.json();
  if (!response.ok) throw Object.assign(new Error(data.error), { status: response.status });
  const viewer = response.headers.get('X-Flock-Viewer');
  if (viewer !== null) responseViewers.set(data, viewer);
  return data;
}
const botQuery = (path, bot, before) => `${path}?${new URLSearchParams(before ? { bot, before } : { bot })}`;
const peerQuery = ({ id: bot, peerId: peer }, before) =>
  `peer-conversation?${new URLSearchParams(before ? { bot, peer, before } : { bot, peer })}`;
const timelineContext = () => view?.dm ?? view;
const timelineKey = (botId, peerId = null) => JSON.stringify([botId, peerId]);
const showingComposer = (bot) => view?.id === bot && !view?.dm;

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
  rememberReadingPosition();
  closeDetail();
  const existing = view?.id === botId ? view : null;
  view = botId ? { id: botId, data: null, events: new Map(), olderBefore: 0, loadedSessions: new Map(),
    sessionsBefore: 0, ...existing, loadingOlder: false, loadingSessions: false,
    dm: peerId ? { id: botId, peerId, events: new Map(), olderBefore: 0 } : null } : null;
  const url = conversationUrl(botId, peerId);
  if (url.href !== location.href) {
    if (updateHistory) history.pushState(null, '', url);
    else history.replaceState(null, '', url);
  }
  save('flock-selected-bot', botId);
  render('bottom');
  const context = view;
  if (context) {
    context.pageRead = Promise.resolve();
    context.refresh = coalescedRefresh((scroll, liveGeneration) =>
      readTimeline(context, () => refreshView(context, scroll, liveGeneration)));
    void context.refresh(context.data ? undefined : 'bottom');
    if (context.dm) {
      context.dm.refresh = coalescedRefresh(scroll => readTimeline(context.dm, () => refreshPeerConversation(context, scroll)));
      void context.dm.refresh('bottom');
    }
  }
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
const sidebarEntries = new Map();
let readState = new Map();
let sidebarViewer = null;

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
function togglePin(botId) {
  if (pins.has(botId)) pins.delete(botId);
  else pins.add(botId);
  save('flock-pins', [...pins]);
  setMenu(null, `menu:${botId}`);
}
let settingsReturnFocus = null;
function openSettingsFromMenu(botId) {
  settingsReturnFocus = botId;
  openDetail({ kind: 'settings' });
}
function openBotSettings(botId) {
  menu = null;
  if (view?.id !== botId) selectBot(botId);
  else renderBots();
  if (view?.data) openSettingsFromMenu(botId);
  else if (view) view.openSettingsOnLoad = botId;
}
function menuButton(botId) {
  const toggle = button(null, 'row-menu', () => (menu === botId ? closeMenu() : setMenu(botId, 'first')));
  toggle.innerHTML = sidebarIcons.more;
  toggle.dataset.focus = `menu:${botId}`;
  toggle.setAttribute('aria-haspopup', 'menu');
  toggle.addEventListener('keydown', (event) => {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    event.preventDefault();
    setMenu(botId, event.key === 'ArrowDown' ? 'first' : 'last');
  });
  return toggle;
}
function menuPopover(bot) {
  const list = node('div', 'menu-popover');
  list.id = 'bot-menu';
  list.setAttribute('role', 'menu');
  list.setAttribute('aria-label', `${bot.name} menu`);
  const actions = [
    [pins.has(bot.id) ? 'Unpin' : 'Pin', sidebarIcons.pin, () => togglePin(bot.id)],
    ['Open settings', sidebarIcons.settings, () => openBotSettings(bot.id)],
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
function createSidebarEntry(botId) {
  const row = node('div');
  const select = button(null, '', () => selectBot(botId));
  select.dataset.focus = `bot:${botId}`;
  const face = node('span');
  const avatarHost = node('span', 'avatar');
  const pinPresence = node('span');
  const pinUnread = node('span', 'unread-dot pin-unread');
  pinUnread.setAttribute('aria-hidden', 'true');
  face.append(avatarHost, pinPresence, pinUnread);

  const name = node('span', 'bot-name');
  const nameText = node('span', 'bot-name-text');
  const presenceDot = node('span', 'presence-dot');
  const rowUnread = node('span', 'unread-dot row-unread');
  rowUnread.setAttribute('aria-hidden', 'true');
  name.append(nameText, presenceDot);
  const preview = node('span', 'bot-preview');
  const dots = node('span', 'working-dots');
  dots.setAttribute('aria-hidden', 'true');
  dots.append(node('i'), node('i'), node('i'));
  const previewText = document.createTextNode('');
  preview.append(dots, previewText);
  const text = node('span', 'bot-text');
  text.append(name, preview);

  const pinName = node('span', 'pin-name');
  const status = node('span', 'sr-only');
  select.append(rowUnread, face, text, pinName, status);
  const tooltip = node('span', 'pin-tooltip');
  tooltip.setAttribute('aria-hidden', 'true');
  const tooltipName = node('span');
  const shortcutLabel = node('kbd');
  tooltip.append(tooltipName, shortcutLabel);
  const toggle = menuButton(botId);
  row.append(select, tooltip, toggle);

  let previousBot;
  let avatarHandle;
  let popover;
  return {
    row,
    update(bot, pinIndex) {
      const pinned = pinIndex >= 0;
      const active = view?.id === botId;
      const expanded = menu === botId;
      row.className = pinned ? 'pin-tile' : 'bot-row';
      row.classList.toggle('selected', active);
      row.classList.toggle('menu-open', expanded);
      select.className = pinned ? 'pin' : 'bot-select';
      if (active) select.setAttribute('aria-current', 'true');
      else select.removeAttribute('aria-current');

      face.className = pinned ? 'pin-face' : '';
      avatarHost.classList.toggle('pin-avatar', pinned);
      avatarHost.classList.toggle('bot-avatar', !pinned);
      tooltip.hidden = pinName.hidden = !pinned;
      text.hidden = pinned;
      const shortcut = pinned && pinIndex < 9 ? pinIndex + 1 : null;
      shortcutLabel.hidden = shortcut === null;
      if (shortcut) {
        shortcutLabel.textContent = `⌥⇧${shortcut}`;
        select.setAttribute('aria-keyshortcuts', `Alt+Shift+${shortcut}`);
      } else select.removeAttribute('aria-keyshortcuts');

      if (previousBot?.name !== bot.name) {
        nameText.textContent = pinName.textContent = tooltipName.textContent = bot.name;
        toggle.setAttribute('aria-label', `${bot.name} menu`);
        popover?.setAttribute('aria-label', `${bot.name} menu`);
      }
      if (!previousBot || previousBot.preview !== bot.preview) {
        previewText.data = bot.preview || 'No conversation yet';
      }
      const hasPresence = activePresence.has(bot.presence);
      presenceDot.hidden = bot.presence !== 'waiting';
      dots.hidden = !busyPresence.has(bot.presence);
      pinPresence.hidden = !pinned || !hasPresence;
      pinPresence.className = hasPresence ? `pin-presence ${bot.presence}` : 'pin-presence';
      const unread = hasUnread(bot);
      row.classList.toggle('unread', !!unread);
      pinUnread.hidden = !pinned || !unread;
      rowUnread.hidden = pinned || !unread;
      const label = [unread && 'New reply', hasPresence && stateLabels[bot.presence]].filter(Boolean).join(', ');
      status.hidden = !label;
      if (status.textContent !== label) status.textContent = label;

      toggle.setAttribute('aria-expanded', expanded);
      if (expanded) {
        toggle.setAttribute('aria-controls', 'bot-menu');
        if (!popover) {
          popover = menuPopover(bot);
          row.append(popover);
        }
      } else if (popover) {
        toggle.removeAttribute('aria-controls');
        popover.remove();
        popover = null;
      }
      if (!previousBot || JSON.stringify(previousBot.avatar) !== JSON.stringify(bot.avatar)) {
        avatarHandle?.destroy();
        avatarHandle = showAvatar(avatarHost, bot, { still: false, state: presenceMotion(bot.presence) });
      } else if (previousBot.presence !== bot.presence) {
        const nextMotion = presenceMotion(bot.presence);
        if (presenceMotion(previousBot.presence) !== nextMotion) avatarHandle?.update({ state: nextMotion });
      }
      previousBot = bot;
    },
    destroy() {
      avatarHandle?.destroy();
      row.remove();
    },
  };
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
  const unreadIds = bots.filter(hasUnread).map((bot) => bot.id);
  if (!changed('bots', bots, [...pins], menu, view?.id, listLoaded, botDraft, unreadIds)) return;
  const focusedSidebarKey = sidebar.contains(document.activeElement) ? document.activeElement.dataset.focus : null;
  const source = sidebarBots();
  const pinned = pinnedBotsInShortcutOrder(source);
  const unpinned = source.filter((bot) => !pins.has(bot.id));
  const activeIds = new Set(source.map((bot) => bot.id));
  for (const [id, entry] of sidebarEntries) {
    if (!activeIds.has(id)) {
      entry.destroy();
      sidebarEntries.delete(id);
    }
  }
  for (const { id } of source) {
    if (!sidebarEntries.has(id)) sidebarEntries.set(id, createSidebarEntry(id));
  }
  const rowFor = ({ id }) => sidebarEntries.get(id).row;
  placeRows(elements.pinned, pinned.map(rowFor));
  elements.pinned.hidden = !pinned.length;
  const list = elements['bot-list'];
  list.setAttribute('aria-busy', !listLoaded);
  const rows = unpinned.map(rowFor);
  if (botDraft?.mode === 'create') rows.unshift(draftRow(botDraft));
  if (!listLoaded) rows.push(...skeletonRows(4, 'skeleton-bot'));
  else if (!bots.length && !botDraft) rows.push(node('p', 'activity-empty', 'No Bots configured'));
  placeRows(list, rows);
  pinned.forEach((bot, index) => sidebarEntries.get(bot.id).update(bot, index));
  unpinned.forEach((bot) => sidebarEntries.get(bot.id).update(bot, -1));
  if (focusedSidebarKey && document.activeElement.dataset.focus !== focusedSidebarKey) focusKey(focusedSidebarKey);
}

function hasUnread(bot) {
  const state = readState.get(bot.id);
  return state && state.incoming > state.readThrough;
}

let readCheckFrame;
function scheduleReadCheck() {
  readCheckFrame ??= requestAnimationFrame(() => {
    readCheckFrame = undefined;
    if (!view?.data || view.dm || sidebarViewer === null || responseViewers.get(view.data) !== sidebarViewer ||
        document.hidden || document.querySelector('dialog[open]')) return;
    const state = readState.get(view.id);
    if (!state || state.incoming <= state.readThrough) return;
    const scroller = elements.conversation;
    if (!scroller.clientHeight) return;
    const message = byId(`event-${state.incoming}`);
    if (!message || !elements.timeline.contains(message)) return;
    const viewport = scroller.getBoundingClientRect();
    const bounds = message.getBoundingClientRect();
    if (bounds.bottom <= viewport.top || bounds.top >= viewport.bottom || bounds.bottom > viewport.bottom + 1) return;
    state.readThrough = state.incoming;
    renderBots();
  });
}

const presenceLabels = { idle: 'Idle', thinking: 'Thinking', working: 'Working', waiting: 'Waiting for input', peer_waiting: 'Waiting for another Bot', stopping: 'Confirming stop', queued: 'Queued' };
const participant = (id) => bots.find((bot) => bot.id === id) ?? { id, name: id };
const botPeers = (id) => participant(id).peers ?? [];

function renderHeader() {
  const bot = view?.data?.bot ?? bots.find((item) => item.id === view?.id);
  const dm = view?.dm;
  const profile = elements.profile;
  elements['profile-heading'].hidden = !bot || !!dm;
  elements['profile-placeholder'].hidden = !!bot || !!dm;
  elements['bot-activity'].hidden = !bot || !!dm;
  elements['peer-picker'].hidden = !view?.data || !!dm || !botPeers(bot.id).length;
  elements['peer-back'].hidden = !dm;
  elements['peer-identity'].hidden = !dm;
  if (dm) {
    elements['peer-back-label'].textContent = `Back to ${bot?.name ?? dm.id}`;
    const pair = [dm.id, dm.peerId].sort().map(participant);
    if (changed('peer-heading', pair.map(({ id, name, avatar }) => [id, name, avatar]))) {
      elements['peer-faces'].replaceChildren(...pair.map((bot) => avatar(bot)));
      elements['peer-heading'].textContent = pair.map(({ name }) => name).join(' & ');
    }
  }
  const nextScopeKey = bot ? JSON.stringify([bot.id, bot.name, bot.avatar]) : '';
  if (nextScopeKey !== scopeKey) {
    scopeAvatar?.destroy();
    scopeAvatar = bot ? showAvatar(elements['activity-scope-avatar'], bot, { still: false }) : null;
    if (bot) elements['activity-scope-name'].textContent = bot.name;
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
  const motion = presenceMotion(presence);
  const look = { state: motion, followPointer: motion === 'idle' };
  const key = JSON.stringify([bot.id, bot.avatar]);
  if (key !== profileKey) {
    profileAvatar?.destroy();
    profileAvatar = showAvatar(elements['profile-avatar'], bot, { still: false, ...look });
    profileKey = key;
    elements.conversation.style.setProperty('--bot-accent', accentColor(bot));
  } else if (profileMotion !== motion) {
    profileAvatar?.update(look);
  }
  profileMotion = motion;
  const offline = connection === 'offline';
  const status = offline ? 'Runtime disconnected' : presenceLabels[presence] ?? presence;
  profile.dataset.presence = presence;
  profile.dataset.state = offline ? 'offline' : presence;
  profile.title = `${bot.name} settings`;
  profile.setAttribute('aria-label', profile.title);
  elements['profile-name'].textContent = bot.name;
  elements['profile-status'].textContent = status;
  elements['profile-status'].hidden = !offline && presence === 'idle';
}

function activityRow(tag, className, title, status, statusClass) {
  const row = node(tag, `activity ${className}`);
  const heading = node('span', 'activity-title', title);
  const line = node('span', `activity-status ${statusClass}`.trim());
  line.append(node('span', 'activity-mark'), node('span', null, status));
  row.append(heading, line);
  return row;
}
function sessionRow(task, session, status, tone = '') {
  const link = runHref(session);
  const row = activityRow(link ? 'a' : 'div', 'session-row', task.request, status, tone);
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
function workflowInspectors(data, open) {
  const sections = [];
  const section = (key, title) => {
    const item = node('details');
    item.dataset.inspector = key;
    item.open = open.has(key);
    item.append(node('summary', null, title));
    sections.push(item);
    return item;
  };
  for (const workflow of data?.workflows ?? []) {
    const item = section(`workflow-${workflow.workflowId}`, `${workflow.request} · ${workflow.status.replaceAll('_', ' ')}`);
    for (const stage of workflow.stages) {
      const status = [stage.status.replaceAll('_', ' ')];
      if (stage.outdated) status.push('Outdated input');
      if (stage.stopRequested && stage.status === 'unreachable') status.push('Stop requested');
      if (stage.lastConfirmedState) status.push(`Last confirmed ${stage.lastConfirmedState}`);
      if (stage.deadlineExceeded) status.push('Soft deadline exceeded');
      const row = node('p', null, [stage.name, ...status].join(' · '));
      const link = linkToRun(stage.runRef);
      if (link) row.append(' · ', link);
      item.append(row);
      if (stage.inputArtifactVersion) item.append(node('p', null, `Pinned input · ${stage.inputArtifactVersion}`));
      if (stage.outputArtifactVersion) item.append(node('p', null, `Output · ${stage.outputArtifactVersion}`));
    }
    for (const check of workflow.verdicts) {
      item.append(node('p', null, `${check.stageId} · ${check.verdict} · ${check.artifactVersion}`));
    }
  }
  for (const summary of data?.compactions ?? []) {
    const item = section(`summary-${summary.compactionId}`, `Conversation summary · ${summary.originalCount} messages`);
    item.append(node('p', null, `Messages ${summary.startMessageId}–${summary.endMessageId}`), node('p', null, summary.summary));
  }
  return sections;
}
function renderActivity() {
  const data = view?.data;
  const loading = !!view && !data;
  const waits = data?.waits ?? [];
  const paused = data?.pausedConversations ?? [];
  if (changed('waits', view?.id, loading, waits, paused)) {
    elements['wait-count'].textContent = waits.length + paused.length;
    elements['wait-count'].hidden = !(waits.length + paused.length);
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
    for (const turn of paused) {
      const row = activityRow('button', 'wait-row', turn.text, waitLabel('model_budget'), 'waiting');
      row.type = 'button';
      row.title = turn.text;
      row.addEventListener('click', () => {
        openNavigation(false);
        openDetail({ kind: 'paused', messageId: turn.messageId });
      });
      rows.push(row);
    }
    elements['wait-list'].replaceChildren(...rows);
    if (loading) elements['wait-list'].append(...skeletonRows(1));
    elements['wait-list'].closest('section').hidden = !loading && !rows.length;
  }
  const groups = [...(view?.loadedSessions.values() ?? [])].filter(task => task.id >= view.sessionsBefore);
  const active = data?.activeSessions ?? [];
  const more = elements['more-sessions'];
  more.hidden = !view?.sessionsBefore;
  more.disabled = !!view?.loadingSessions;
  if (!changed('sessions', view?.id, loading, groups, active, data?.workflows, data?.compactions)) return;
  const key = (taskId, run) => JSON.stringify([taskId, run.environmentId, run.threadId]);
  const activeKeys = new Set();
  const live = [];
  for (const run of active) {
    const identity = key(run.taskId, run);
    if (activeKeys.has(identity)) continue;
    activeKeys.add(identity);
    live.push(sessionRow({ request: run.request, createdAt: run.createdAt }, run, stateLabels[run.state] ?? run.state,
      run.state === 'waiting' ? 'waiting' : 'live'));
  }
  const ended = [];
  for (const task of groups.sort((a, b) => b.id - a.id)) {
    for (const session of task.sessions) {
      if (activeKeys.has(key(task.id, session))) continue;
      const status = Object.hasOwn(sessionEndLabels, session.state) ? sessionEndLabels[session.state] : sessionEndLabels.ended;
      ended.push(sessionRow(task, session, status, session.state === 'failed' ? 'failed' : ''));
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
  const open = new Set(Array.from(list.querySelectorAll('details[open]'), (item) => item.dataset.inspector));
  rows.push(...workflowInspectors(data, open));
  list.setAttribute('aria-busy', loading);
  list.replaceChildren(...rows);
  if (loading) list.append(...skeletonRows(2));
  list.closest('section').hidden = !loading && !rows.length && more.hidden;
}

function beginReply(target, text = '') {
  const composer = view?.composer;
  if (!target.messageId || !composer || sendingMessages.has(composer)) return;
  if (view.dm) selectBot(view.id);
  composer.openReply(target, text);
  closeDetail();
  renderComposer();
  elements.message.focus();
}
const icons = {
  copy: '<path d="M9 9.5A1.5 1.5 0 0 1 10.5 8h7A1.5 1.5 0 0 1 19 9.5v9a1.5 1.5 0 0 1-1.5 1.5h-7A1.5 1.5 0 0 1 9 18.5z"/><path d="M15 8V5.5A1.5 1.5 0 0 0 13.5 4h-7A1.5 1.5 0 0 0 5 5.5v9A1.5 1.5 0 0 0 6.5 16H9"/>',
  copied: '<path d="m5.5 12.5 4 4 9-9"/>',
  reply: '<path d="M9.5 7 4.5 12l5 5"/><path d="M5 12h9a5.5 5.5 0 0 1 5.5 5.5V19"/>',
  forward: '<path d="m14.5 7 5 5-5 5"/><path d="M19 12h-9a5.5 5.5 0 0 0-5.5 5.5V19"/>',
  resultWarning: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.8v4.8m0 3.45h.01"/>',
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
  control.dataset.timelineAction = name;
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
const questionKey = (task) => JSON.stringify([task.id, task.wait.id, task.currentRevision ?? 0, task.generation]);

function questionActions(context, task, { dialog = false } = {}) {
  const target = { kind: 'wait', messageId: task.wait.messageId, summary: waitText(task.wait) };
  const actions = node('div', dialog ? 'wait-primary' : 'event-actions question-actions');
  const choices = task.wait.choices ?? [];
  if (choices.length) {
    const key = questionKey(task);
    const selections = context.questionChoices ??= new Map();
    actions.append(questionChoiceForm({
      key, prompt: target.summary, choices, dialog, answer: selections.get(key), busy: pendingWaits.has(task.wait.id),
      select(answer) {
        selections.set(key, answer);
        for (const form of document.querySelectorAll('.question-choice-form')) {
          if (form.dataset.questionKey === key) updateQuestionChoiceForm(form, answer, pendingWaits.has(task.wait.id));
        }
      },
      submit(answer) {
        const current = view === context && context.data?.waits.find((item) => item.wait.id === task.wait.id);
        if (current && questionKey(current) === key) void operate(context, current, 'answer', { text: answer });
      },
    }));
  }
  const reply = button('Write a reply', dialog ? 'dialog-button quiet' : 'text-button', () => {
    if (view?.id === context.id) beginReply(target);
  });
  reply.disabled = pendingWaits.has(task.wait.id);
  actions.append(reply);
  return actions;
}
function readOnlyChoices(choices) {
  const options = node('ul', 'read-only-choices');
  for (const choice of choices) options.append(node('li', null, choice));
  return options;
}
function pausedActions(turn) {
  const actions = node('div', 'event-actions');
  const waitMessageId = turn.messageId;
  const key = `message/${waitMessageId}`;
  const approve = button('Approve one more model call', 'button', () => void submitOperation(view, key, {
    itemKind: 'conversation_model_budget', itemId: turn.replyToMessageId, waitId: waitMessageId,
    taskId: 0, targetRevision: 0, targetGeneration: waitMessageId, decision: 'approve',
  }));
  approve.disabled = pendingWaits.has(key);
  actions.append(approve);
  return actions;
}
function suggestionActions(event) {
  const actions = node('div', 'event-actions');
  const target = { kind: 'message', messageId: event.id, summary: event.data.text };
  for (const suggestion of event.data.suggestions)
    actions.append(button(suggestion, 'button', () => beginReply(target, suggestion)));
  return actions;
}
function messageSide(event) {
  if (event.kind === 'cancelled') return 'system';
  return ['conversation_message', 'user_answer'].includes(event.kind) || (event.kind === 'request' && event.data.source !== 'automation') ? 'user' : 'bot';
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
  time.title = [fullTime.format(timestamp), extra].filter(Boolean).join(' · ');
  return time;
}
function sourceAction(source, label = 'Open source') {
  const control = button(label, 'text-button', async () => {
    const match = byId(`event-${source.messageId}`);
    const target = match?.closest('article') ?? match;
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
  control.dataset.timelineAction = `source:${source.messageId}`;
  return control;
}
function runIcon(run) {
  const link = linkToRun(run);
  if (!link) return null;
  link.className = 'run-link';
  link.dataset.timelineAction = `run:${link.href}`;
  link.title = 'Open in T3';
  link.setAttribute('aria-label', 'Open in T3');
  link.replaceChildren(icon('open'));
  return link;
}
function eventCard(event, context, place, report) {
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
  if (report) {
    const anchor = node('span');
    anchor.id = `event-${report.id}`;
    anchor.hidden = true;
    row.append(anchor);
  }
  row.classList.toggle('group-start', place.groupStart);
  row.classList.toggle('task-start', place.taskStart);
  const trigger = event.kind === 'request' ? data.trigger : null;
  let scheduled = '';
  if (trigger?.targetTime) {
    scheduled = trigger.occurrenceCount > 1
      ? `Covers ${fullTime.format(trigger.coveredFrom * 1000)} to ${fullTime.format(trigger.coveredTo * 1000)}`
      : `Scheduled for ${fullTime.format(trigger.targetTime * 1000)}`;
  } else if (trigger?.source) scheduled = `Webhook from ${trigger.source}`;
  const showMeta = side === 'bot' ? place.groupStart || place.taskStart : place.taskStart;
  const meta = node('div', showMeta ? 'msg-meta' : 'msg-meta sr-only');
  const name = node('span', side === 'bot' && place.groupStart ? 'msg-name' : 'msg-name sr-only',
    side === 'bot' ? bot?.name ?? 'Bot' : 'You');
  meta.append(name, eventTime(event, scheduled));
  row.append(meta);

  const line = node('div', 'msg-line');
  if (side === 'bot') line.append(node('span', 'msg-avatar-space'));
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
    const choices = pendingWait?.wait.choices ?? data.choices;
    if (!context.canDecide && choices?.length) bubble.append(readOnlyChoices(choices));
    if (context.canDecide && pendingWait?.wait.reason === 'user_question') extras.push(questionActions(view, pendingWait));
    else if (pendingWait) {
      const open = node('div', 'event-actions question-actions');
      open.append(button('View pending request', 'button', () => openDetail({ kind: 'wait', taskId: pendingWait.id, waitId: data.id })));
      extras.push(open);
    } else {
      const resolution = node('p', 'msg-resolution');
      resolution.append(icon('check'), document.createTextNode(closedLabels[context.closed.get(data.id)] ?? 'Request closed'));
      extras.push(resolution);
      if (data.reason === 'user_question' && view.composer?.textAt(event.id)) {
        extras.push(button('View reply draft', 'text-button', () => beginReply({
          kind: 'wait', messageId: event.id, summary: copy,
        })));
      }
    }
  } else if (event.kind === 'conversation_reply') {
    text.textContent = data.text;
    bubble.append(text);
    const paused = context.paused.get(data.messageId);
    if (paused && context.canDecide) extras.push(pausedActions(paused));
    else if (data.suggestions?.length) {
      if (context.canDecide) extras.push(suggestionActions(event));
      else bubble.append(readOnlyChoices(data.suggestions));
    }
    if (!paused) replyTarget = ['Reply to this message', { kind: 'message', messageId: event.id, summary: data.text }];
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
    const outcome = data.runStatus;
    if (outcome === 'completed') {
      text.append(icon('check', 'glyph kind-icon result-success'), node('span', 'sr-only', 'Session complete: '));
    } else if (['failed', 'cancelled', 'interrupted', 'rolled_back'].includes(outcome)) {
      const tone = outcome === 'failed' ? 'result-failed' : 'result-interrupted';
      text.append(icon('resultWarning', `glyph kind-icon ${tone}`), node('span', 'sr-only', `Session ${stateLabels[outcome].toLowerCase()}: `));
    }
    text.append(document.createTextNode(data.text ?? ''));
    const link = runIcon(data.run);
    if (link) text.append(link);
    bubble.append(text);
    if (data.text) replyTarget = ['Reply to this result', { kind: 'result', messageId: event.id, summary: data.text }];
    if (data.reason) bubble.append(reasonText(data.reason));
  } else {
    if (trigger) text.append(icon('clock', 'glyph kind-icon'), node('span', 'sr-only', 'Automated request: '));
    text.append(document.createTextNode(data.text ?? ''));
    bubble.append(text);
    if (event.kind === 'conversation_message' && data.imageAttachmentCount > 0) {
      const count = data.imageAttachmentCount;
      bubble.append(node('p', 'message-attachments', count === 1 ? '1 image attached' : `${count} images attached`));
    }
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
  for (const file of data.attachments ?? []) {
    const link = node('a', 'button', file.name);
    link.href = '/api/channels/attachments/' + encodeURIComponent(file.id);
    link.download = file.name;
    if (file.mimeType.startsWith('image/')) {
      const preview = node('img', 'channel-image');
      preview.src = link.href;
      preview.alt = file.name;
      preview.loading = 'lazy';
      link.replaceChildren(preview);
    }
    extras.push(link);
  }
  for (const artifact of data.artifacts ?? []) {
    const link = node('a', 'button', artifact.title);
    link.href = '/api/artifacts/' + encodeURIComponent(artifact.id);
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
    extras.push(link);
  }
  if (copy) {
    const control = iconButton('copy', 'Copy text', () => void copyMessage(control, copy));
    actions.append(control);
  }
  if (context.canDecide && pendingWait?.wait.reason === 'user_question') {
    const reply = iconButton('reply', 'Write a reply', () => beginReply({ kind: 'wait', messageId: pendingWait.wait.messageId, summary: waitText(pendingWait.wait) }));
    reply.disabled = pendingWaits.has(pendingWait.wait.id);
    actions.append(reply);
  }

  if (context.canDecide && replyTarget) {
    const [label, target] = replyTarget;
    actions.append(iconButton('reply', label, () => beginReply(target)));
  }
  line.append(bubble, actions);
  row.append(line, ...extras);
  return row;
}
function syncTimelineAvatar(row, show, bot) {
  const current = row.querySelector('.msg-line')?.firstElementChild;
  if (show && current?.classList.contains('msg-avatar-space')) current.replaceWith(avatar(bot, 'msg-avatar'));
  else if (!show && current?.classList.contains('msg-avatar')) current.replaceWith(node('span', 'msg-avatar-space'));
}
function workingLine(data) {
  const row = node('div', 'msg bot working group-start');
  const line = node('div', 'msg-line');
  const dots = node('span', 'typing');
  dots.setAttribute('aria-hidden', 'true');
  dots.append(node('span'), node('span'), node('span'));
  let label = presenceLabels.thinking;
  if (data.presence === 'working') {
    label = data.activeSessions?.[0]?.request ?? presenceLabels.working;
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

function peerExchangeLink(peer, messageCount, activeBot, firstMessageId) {
  const count = messageCount === 1 ? '1 message' : `${messageCount} messages`;
  const link = peerLink(activeBot, peer.id, null, 'peer-exchange-link');
  link.append(avatar(peer, 'peer-exchange-avatar'), node('span', null, `${count} with ${peer.name}`));
  link.dataset.peerFocus = firstMessageId;
  return link;
}

function peerThread(events, dm, keep) {
  if (!events.length) {
    const title = dm.error ? 'DM unavailable' : dm.newestEvent == null ? 'Loading…' : 'No messages yet';
    return [keep('empty', [title, dm.error], () => {
      const empty = node('div', 'empty-state');
      empty.append(node('h2', null, title));
      if (dm.error) empty.append(node('p', null, dm.error));
      return empty;
    })];
  }
  const rows = [];
  let day = '';
  for (const event of events) {
    const nextDay = dayLabel(event.time);
    if (nextDay !== day) {
      rows.push(keep(`day-${event.id}`, nextDay, () => node('p', 'day', nextDay)));
      day = nextDay;
    }
    const from = participant(event.data.fromBotId);
    rows.push(keep(`event-${event.id}`, [event, from.name, from.avatar], () => peerMessage(event, from)));
  }
  return rows;
}
function peerMessage(event, from) {
  const { text, forwardedMessage } = event.data;
  const row = node('article', 'msg group-start peer-dm-message');
  row.id = `event-${event.id}`;
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
  return row;
}

function timelineScrollPosition() {
  const scroller = elements.conversation;
  const height = scroller.scrollHeight;
  const top = scroller.scrollTop;
  return { height, top, atBottom: height - top - scroller.clientHeight < 65 };
}

function rememberReadingPosition() {
  const context = timelineContext();
  if (context?.newestEvent == null || (!context.peerId && !context.data)) return;
  const { top, atBottom } = timelineScrollPosition();
  if (atBottom) return;
  const viewportTop = elements.conversation.getBoundingClientRect().top;
  const anchors = elements.timeline.querySelectorAll(':scope > [id], :scope > [data-peer-focus]');
  const anchor = [...anchors].find((row) => row.getBoundingClientRect().bottom > viewportTop);
  const { events, olderBefore, newestEvent } = context;
  readingPositions.set(timelineKey(context.id, context.peerId), {
    // Retain loaded pages so an older message is available when its conversation reopens.
    history: { events, olderBefore, newestEvent, historyCursor: context.data?.historyCursor },
    top,
    anchorId: anchor?.id,
    peerFocus: anchor?.dataset.peerFocus,
    offset: anchor ? anchor.getBoundingClientRect().top - viewportTop : 0,
  });
}

function restoreReadingPosition(position) {
  const scroller = elements.conversation;
  const anchor = position.anchorId ? byId(position.anchorId) :
    [...elements.timeline.querySelectorAll('[data-peer-focus]')].find((row) => row.dataset.peerFocus === position.peerFocus);
  scroller.scrollTop = anchor ? scroller.scrollTop + anchor.getBoundingClientRect().top -
    scroller.getBoundingClientRect().top - position.offset : position.top;
}

function eventHistoryForRefresh(context) {
  // Reopen saved pages only after a successful read of the current conversation.
  if (context.newestEvent != null) return context;
  const cached = readingPositions.get(timelineKey(context.id, context.peerId));
  return cached ? { ...cached.history } : context;
}

function updateJumpToLatest() {
  const scroller = elements.conversation;
  const frame = scroller.parentElement;
  const { height, top, atBottom } = timelineScrollPosition();
  frame.toggleAttribute('data-history-above', top > 1);
  frame.toggleAttribute('data-history-below', height - top - scroller.clientHeight > 1);
  const control = elements['jump-to-latest'];
  if (control.hidden === atBottom) return;
  if (atBottom && document.activeElement === control) scroller.focus({ preventScroll: true });
  control.hidden = atBottom;
}

function renderTimeline(scroll = 'auto', previousScroll = timelineScrollPosition(), liveAfterId) {
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
  const key = context?.newestEvent != null && timelineKey(context.id, context.peerId);
  const position = key && readingPositions.get(key);
  const moveScroll = () => {
    if (scroll === 'older') scroller.scrollTop = previousScroll.top + scroller.scrollHeight - previousScroll.height;
    else if (position) {
      // The restored offset already includes the returning composer's height.
      composerHeight = elements['composer-container'].offsetHeight;
      restoreReadingPosition(position);
      readingPositions.delete(key);
    } else if (scroll === 'bottom' || previousScroll.atBottom) scroller.scrollTop = scroller.scrollHeight;
    else scroller.scrollTop = previousScroll.top;
    updateJumpToLatest();
  };
  if (view?.dm) {
    const dm = view.dm;
    const participants = [participant(dm.id), participant(dm.peerId)].map(({ id, name, avatar }) => [id, name, avatar]);
    if (!changed('timeline', dm.id, dm.peerId, events, dm.newestEvent, dm.error, participants, dayFormat.format(Date.now()))) return;
    scroller.setAttribute('aria-label', 'Bot DM history');
    updateTimelineRows(dm, (keep) => peerThread(events, dm, keep));
    moveScroll();
    return;
  }
  if (changed('timeline', view?.id, data?.composerPerson, data?.bot, events, data?.waits, data?.queued, [...pendingWaits], listLoaded,
    data?.presence, data?.activeSessions, data?.sessionTasks, data?.pausedConversations, bots, dayFormat.format(Date.now()))) {
    scroller.setAttribute('aria-label', 'Conversation history');
    updateTimelineRows(context, (keep) => conversationRows(data, events, keep));
    moveScroll();
  }
  const canAnimate = liveAfterId != null && scroll === 'auto' && previousScroll.atBottom && !document.hidden &&
    !reducedMotion.matches && !document.querySelector('dialog[open]');
  if (canAnimate) {
    const id = replyArrivalId(events, liveAfterId);
    if (id != null) animateLiveReply(id);
  }
}

function conversationRows(data, events, keep) {
  const timeline = {
    bot: data?.bot ?? bots.find((item) => item.id === view?.id),
    canDecide: data?.canDecide === true,
    paused: new Map((data?.pausedConversations ?? []).map(item => [item.messageId, item])),
    waits: new Map((data?.waits ?? []).map((task) => [task.wait.id, task])),
    queued: new Map((data?.queued ?? []).map((task) => [task.id, task])),
    closed: new Map(events.filter((event) => event.kind === 'wait_closed').map((event) => [event.data.waitId, event.data.resolution])),
  };
  const bot = timeline.bot;
  const botVersion = JSON.stringify([bot?.id, bot?.name, bot?.avatar]);
  const shown = timelineMessages(events, dayLabel);
  const rows = [];
  let peerMessages = [];
  const appendPeerMessages = () => {
    if (!peerMessages.length) return;
    const [first] = peerMessages;
    const { fromBotId, toBotId } = first.data;
    const peer = participant(fromBotId === view.id ? toBotId : fromBotId);
    const count = peerMessages.length;
    rows.push(keep(`peer-${first.id}`, [peer.id, peer.name, peer.avatar, count],
      () => peerExchangeLink(peer, count, view.id, first.id)));
    peerMessages = [];
  };
  let day = '';
  let previous = null;
  shown.forEach(({ event, report }, index) => {
    const eventDay = dayLabel(event.time);
    if (eventDay !== day || event.kind !== 'bot_message' || peerMessages[0]?.data.exchangeId !== event.data.exchangeId) appendPeerMessages();
    if (eventDay !== day) {
      rows.push(keep(`day-${event.id}`, eventDay, () => node('p', 'day', eventDay)));
      day = eventDay;
      previous = null;
    }
    if (event.kind === 'bot_message') { peerMessages.push(event); previous = null; return; }
    const side = messageSide(event);
    const next = shown[index + 1]?.event;
    const place = {
      groupStart: previous?.side !== side,
      groupEnd: !next || next.kind === 'bot_message' || messageSide(next) !== side || dayLabel(next.time) !== day,
      taskStart: previous?.taskId !== event.taskId,
      afterQuestion: previous?.kind === 'waiting' && previous.waitId === event.data.waitId,
    };
    const wait = event.kind === 'waiting' ? timeline.waits.get(event.data.id) : null;
    const paused = event.kind === 'conversation_reply' ? timeline.paused.get(event.data.messageId) : null;
    const blocker = ['request', 'conversation_message'].includes(event.kind) ? timeline.queued.get(event.taskId)?.blockedBy : null;
    const blockedOwner = blocker && bots.find((item) => item.id === blocker.botId)?.name;
    const row = keep(`event-${event.id}`,
      [event, report, botVersion, timeline.canDecide, place.groupStart, place.taskStart, place.afterQuestion, wait,
        event.kind === 'waiting' ? timeline.closed.get(event.data.id) : null,
        wait && pendingWaits.has(wait.wait.id), paused, paused && pendingWaits.has(`message/${paused.messageId}`), blocker, blockedOwner,
        event.kind === 'waiting' && !!view.composer?.textAt(event.id)],
      () => eventCard(event, timeline, place, report));
    if (side === 'bot') syncTimelineAvatar(row, place.groupEnd && bot, bot);
    rows.push(row);
    previous = { side, taskId: event.taskId, kind: event.kind, waitId: event.data.id };
  });
  appendPeerMessages();
  if (!rows.length) {
    const loaded = !!data || (!view && listLoaded);
    rows.push(keep('empty', [!!data, loaded, !bots.length, botVersion], () => {
      const empty = node('div', 'empty-state');
      if (data?.bot) empty.append(avatar(data.bot));
      empty.append(node('h2', null, loaded ? (data ? data.bot.name : 'No Bots') : 'Loading…'));
      if (!data && loaded && !bots.length) {
        empty.append(node('p', null, 'Create a Bot to get started.'));
        empty.append(button('New bot', 'dialog-button primary', () => openNewBot(elements['new-bot-button'])));
      }
      return empty;
    }));
  }
  if (data && (data.presence === 'working' || data.presence === 'thinking')) {
    rows.push(keep('working', [botVersion, data.presence, data.activeSessions?.[0]?.request], () => workingLine(data)));
  }
  return rows;
}

function setNotice(context, text, isError = false) {
  notices.set(context.id, { text, isError });
  if (showingComposer(context.id)) renderComposer();
}
const replyLabels = { wait: 'Reply to a question', message: 'Reply to a message', result: 'Reply to a result' };
const composerLimit = (reply) => reply?.kind === 'wait' ? 8000 : 16000;
const screenshotAnchor = (reply) => reply?.messageId ?? 0;
const screenshotsFor = (composer) => screenshotDrafts.get(composer);
let pickerTarget = null;
let renderedScreenshots = null;

function clearScreenshots(composer, reply) {
  const drafts = screenshotsFor(composer);
  const anchor = screenshotAnchor(reply);
  const draft = drafts?.get(anchor);
  drafts?.delete(anchor);
  for (const image of draft?.images ?? []) URL.revokeObjectURL(image.preview);
}

async function addScreenshots(context, composer, reply, files) {
  const anchor = screenshotAnchor(reply);
  const isCurrent = () => view === context && context.composer === composer && context.data.canDecide === true && !context.dm &&
    screenshotAnchor(composer.reply) === anchor && !closedQuestion(context, reply) && !sendingMessages.has(composer);
  if (!files.length || !composer || !isCurrent()) return;
  let drafts = screenshotsFor(composer);
  if (!drafts) screenshotDrafts.set(composer, drafts = new Map());
  const draft = drafts.get(anchor) ?? { images: [] };
  if (draft.loading) return;
  drafts.set(anchor, draft);
  draft.loading = true;
  draft.error = '';
  notices.delete(context.id);
  renderComposer();
  try { await stageScreenshots(draft, files, isCurrent); }
  finally {
    draft.loading = false;
    if (showingComposer(context.id)) renderComposer();
  }
}

function renderScreenshots(draft, disabled) {
  const list = elements['screenshot-previews'];
  const images = draft?.images ?? null;
  if (renderedScreenshots !== images) {
    renderedScreenshots = images;
    list.replaceChildren(...(images ?? []).map((image, index) => {
      const item = node('li', 'screenshot-preview');
      const preview = node('img');
      preview.alt = image.attachment.name;
      preview.title = image.attachment.name;
      preview.src = image.preview;
      const remove = button('×', 'icon-button remove-screenshot', () => {
        const hadFocus = document.activeElement === remove;
        URL.revokeObjectURL(image.preview);
        draft.images = draft.images.filter((entry) => entry !== image);
        draft.error = '';
        renderComposer();
        if (hadFocus) {
          const buttons = list.querySelectorAll('button');
          const next = buttons[Math.min(index, buttons.length - 1)] ?? elements['attach-screenshots'];
          (next.disabled ? elements.message : next).focus();
        }
      });
      remove.setAttribute('aria-label', `Remove ${image.attachment.name}`);
      remove.title = `Remove ${image.attachment.name}`;
      item.append(preview, remove);
      return item;
    }));
  }
  list.hidden = !images?.length;
  for (const button of list.querySelectorAll('button')) button.disabled = disabled;
}

const closedQuestion = (context, reply) => reply?.kind === 'wait' &&
  !context.data.waits.some((task) => task.wait.messageId === reply.messageId);
let composition = null;
function scheduleNoticeFade(context, notice) {
  const bot = context.id;
  if (!notice || notice.isError || notice.timer || sendingMessages.has(context.composer) || [...pendingWaits.values()].includes(bot)) return;
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
  const readOnly = context?.data?.canDecide === false;
  const available = !!context?.data && !context.dm && (!!context.composer || readOnly);
  const canDecide = available && context.data.canDecide === true;
  elements['composer-container'].hidden = !available;
  elements.composer.hidden = !canDecide;
  elements['view-only-notice'].hidden = !available || canDecide;
  if (!canDecide || !context.composer) {
    composition = null;
    elements.message.value = '';
    return renderScreenshots(null, true);
  }
  const { composer } = context;
  const reply = composer.reply;
  if (composition && (composition.composer !== composer || composition.anchor !== reply?.messageId)) composition = null;
  const screenshots = screenshotsFor(composer)?.get(screenshotAnchor(reply));
  const notice = notices.get(context.id);
  const body = composer.text;
  const busy = sendingMessages.has(composer);
  const closedReply = closedQuestion(context, reply);
  const source = reply && context.events.get(reply.messageId)?.data;
  const summary = reply?.summary ?? (source && (reply.kind === 'wait' ? waitText(source) : source.report ?? source.text));
  elements['reply-context'].hidden = !reply;
  elements['reply-label'].textContent = reply ? replyLabels[reply.kind] : '';
  elements['reply-question'].textContent = reply ? summary ?? 'Earlier message' : '';
  elements['compose-label'].textContent = reply ? replyLabels[reply.kind] : 'Message';
  elements['cancel-reply'].disabled = busy;
  elements.message.placeholder = reply ? 'Write a reply' : `Message ${context.data.bot.name}`;
  if (!composition && elements.message.value !== body) {
    elements.message.value = body;
    fitMessage();
  }
  elements.message.readOnly = busy || closedReply;
  elements['attach-screenshots'].disabled = busy || closedReply;
  elements['attach-screenshots'].setAttribute('aria-disabled', String(!!screenshots?.loading));
  renderScreenshots(screenshots, busy || !!screenshots?.loading);
  elements.composer.setAttribute('aria-busy', String(busy));
  elements.send.disabled = busy || !!composition || closedReply || !!screenshots?.loading || !body.trim() || byteLength(body) > composerLimit(reply);
  elements.composer.classList.toggle('can-send', !elements.send.disabled);
  elements.send.setAttribute('aria-label', reply ? 'Send reply' : 'Send message');
  elements.send.title = reply ? 'Reply' : 'Send';
  const screenshotError = !busy && !closedReply && screenshots?.error;
  elements.notice.textContent = notice?.isError ? notice.text :
    (closedReply ? 'This question is closed. You can still copy your reply.' : screenshotError || notice?.text || '');
  elements.notice.classList.toggle('error', !!(notice?.isError || screenshotError));
  elements.notice.classList.toggle('fading', !closedReply && !screenshotError && !!notice?.fading);
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
    await Promise.all([refreshBots(), view?.id === savedBot.id && view.refresh()]);
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
  closeDetail();
  botEditor.openCreate({ bots, origin });
}

function openEditAppearance(bot) {
  reopenSettingsOnClose = true;
  closeDetail();
  botEditor.openEdit(bot, { bots, origin: elements.profile });
}

function generalContent(bot) {
  const hero = node('div', 'settings-hero');
  const face = node('span', 'settings-avatar');
  detailAvatar = { mount: () => showAvatar(face, bot, { still: false }) };
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
  const rows = node('div', 'settings-card settings-rows');
  if (!bot.soul) rows.append(settingsRow('Role', 'None'));
  rows.append(bot.cron.length
    ? settingsRow('Recurring requests', String(bot.cron.length), () => selectSection('cron', true))
    : settingsRow('Recurring requests', 'None'));
  const access = document.createElement('a');
  access.className = 'settings-row';
  access.href = '/settings/' + encodeURIComponent(bot.id) + '/access';
  access.append(node('span', 'settings-row-label', 'Access and credentials'));
  access.insertAdjacentHTML('beforeend', chevron);
  rows.append(access);
  if (!bot.soul) return [hero, rows];
  const role = node('section', 'settings-card');
  role.setAttribute('aria-label', 'Role');
  role.append(node('p', 'settings-soul', bot.soul));
  return [hero, role, rows];
}
function cronContent(bot) {
  const list = node('div', 'settings-card cron-list');
  for (const item of bot.cron) {
    const expression = item.schedule;
    const readable = describeCron(expression);
    const row = node('div', 'cron-row');
    const when = node('p', 'cron-when');
    when.append(readable ? node('span', 'cron-schedule', readable) : node('code', 'cron-expression', expression));
    when.append(node('span', 'cron-zone', item.timezone));
    if (readable) when.append(node('code', 'cron-expression', expression));
    row.append(when, node('p', 'cron-request', item.request));
    list.append(row);
  }
  return [list];
}
function settingsContent(bot, section = 'general') {
  // Without recurring requests there are no section tabs, so "General" would name nothing.
  elements['detail-title'].textContent = section === 'general' && !bot.cron.length ? 'Settings' : settingsSections[section][0];
  if (section === 'cron') return cronContent(bot);
  return generalContent(bot);
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
  const canDecide = context.data.canDecide === true;
  elements['detail-title'].textContent = waitLabel(wait.reason);
  const request = node('blockquote', 'wait-request');
  request.append(node('p', 'wait-request-text', task.request));
  if (task.createdAt) request.append(node('p', 'wait-request-meta', `Started ${fullTime.format(task.createdAt * 1000)}`));
  const reason = node('div', 'wait-reason');
  reason.append(node('p', 'wait-text', waitText(wait)));
  if (wait.decisionReason) reason.append(node('p', 'wait-why', wait.decisionReason));
  if (wait.reason === 'deadline' && task.deadline)
    reason.append(node('p', 'wait-why', `Current deadline · ${fullTime.format(task.deadline * 1000)}`));
  const content = [request, reason];
  const actions = node('div', 'wait-actions');
  const primary = node('div', 'wait-primary');
  const perform = (operation, fields) => void operate(context, task, operation, fields);
  const action = (label, operation) => button(label, 'dialog-button primary', () => perform(operation));
  const approveInT3 = canDecide && wait.reason === 't3_input';
  const runLink = linkToRun(wait.run, approveInT3 ? 'Approve in T3' : 'Open in T3');
  if (runLink) {
    runLink.className = approveInT3 ? 'dialog-button primary' : 'dialog-button quiet';
    primary.append(runLink);
  }
  if (!canDecide) {
    if (wait.choices?.length) reason.append(readOnlyChoices(wait.choices));
    if (runLink) content.push(primary);
    return content;
  }
  if (wait.reason === 'user_question') {
    primary.append(questionActions(context, task, { dialog: true }));
  } else if (wait.reason === 'budget') primary.append(action('Approve one more run', 'approve'));
  else if (wait.reason === 'model_budget') primary.append(action('Approve one more model call', 'approve'));
  else if (wait.reason === 'model_failure') primary.append(action('Retry decision', 'retry'));
  else if (wait.reason === 'deadline') {
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
  if (detail.kind === 'paused') {
    const turn = view.data.pausedConversations.find((item) => item.messageId === detail.messageId);
    if (changed('detail', detail, turn, view.data.canDecide, [...pendingWaits])) {
      elements['detail-title'].textContent = waitLabel('model_budget');
      elements['detail-body'].replaceChildren(node('p', 'prose', turn?.text ?? 'This request has closed.'));
      if (turn && view.data.canDecide === true) elements['detail-body'].append(pausedActions(turn));
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
  const busy = pendingWaits.has(task.wait.id);
  for (const control of elements['detail-body'].querySelectorAll('button, input')) {
    if (!control.closest('.question-choice-form')) control.disabled = busy;
  }
  const choiceForm = elements['detail-body'].querySelector('.question-choice-form');
  if (choiceForm) updateQuestionChoiceForm(choiceForm, view.questionChoices?.get(questionKey(task)), busy);
}
function render(scroll, liveAfterId) {
  renderBots();
  renderHeader();
  renderActivity();
  const previousScroll = timelineScrollPosition();
  renderComposer();
  renderTimeline(scroll, previousScroll, liveAfterId);
  renderDetail();
  scheduleReadCheck();
}

function mergeSessionTasks(context, tasks) {
  for (const task of tasks) {
    // A history page can arrive after a live update for the same task.
    if (task.generation >= (context.loadedSessions.get(task.id)?.generation ?? 0))
      context.loadedSessions.set(task.id, task);
  }
}

function mergeLatest(context, latest) {
  if (context.data?.canDecide !== latest.canDecide) {
    signatures.delete('timeline');
    signatures.delete('detail');
  }
  if (!context.data) context.sessionsBefore = latest.sessionsBefore;
  mergeSessionTasks(context, latest.sessionTasks);
  context.data = latest;
  if (context.questionChoices?.size) {
    const current = new Set(latest.waits.map(questionKey));
    for (const key of context.questionChoices.keys()) if (!current.has(key)) context.questionChoices.delete(key);
  }
  const composer = context.composer = latest.canDecide === true ? composerDrafts(context.id, latest.composerPerson) : null;
  if (!composer) return;
  if (!sendingMessages.has(composer) && closedQuestion(context, composer.reply) && !composer.text &&
      !screenshotsFor(composer)?.get(screenshotAnchor(composer.reply))?.images.length) {
    composer.closeReply();
    setNotice(context, 'That question is no longer open, so your reply was not sent.');
  }
}
async function conversationPage(timeline, before) {
  if (!timeline.peerId) return api(botQuery('conversation', timeline.id, before));
  const { messages, hasOlder } = await api(peerQuery(timeline, before));
  return { events: messages, olderBefore: hasOlder ? messages[0].id : 0 };
}
async function refreshPeerConversation(context, scroll) {
  if (context !== view) return;
  const dm = context.dm;
  const isCurrent = () => context === view;
  const readPage = before => conversationPage(dm, before);
  try {
    const latest = await readPage();
    if (!isCurrent()) return;
    const eventHistory = eventHistoryForRefresh(dm);
    await refreshEventHistory(eventHistory, latest, readPage, isCurrent);
    if (!isCurrent()) return;
    if (eventHistory !== dm) Object.assign(dm, eventHistory);
    dm.error = null;
  } catch (error) {
    if (!isCurrent()) return;
    if (dm.newestEvent == null) dm.error = errors[error.message] ?? 'Could not load this conversation.';
    if (![403, 404].includes(error.status)) retrySync();
  }
  renderTimeline(scroll);
}
async function refreshView(context, scroll, liveGeneration) {
  const isCurrent = () => context === view &&
    (liveGeneration == null || (liveGeneration === visibilityGeneration && !document.hidden));
  if (!isCurrent()) return;
  const liveAfterId = liveGeneration == null ? undefined : context.newestEvent;
  try {
    const params = new URLSearchParams({ bot: context.id });
    const eventHistory = eventHistoryForRefresh(context);
    const historyCursor = context.data?.historyCursor ?? eventHistory.historyCursor;
    if (historyCursor != null) params.set('changesAfter', historyCursor);
    if (context.data) params.set('sessionsAfter', context.data.sessionCursor);
    const latest = await api(`view?${params}`);
    if (!isCurrent()) return;
    await refreshEventHistory(eventHistory, latest, before => conversationPage(context, before), isCurrent);
    if (!isCurrent()) return;
    if (eventHistory !== context) Object.assign(context, eventHistory);
    mergeLatest(context, latest);
    setConnection('ready');
  } catch (error) {
    if (!isCurrent()) return;
    if ([401, 403, 404].includes(error.status)) {
      context.composer = null;
      renderComposer();
    }
    if (error.status === 404) void refreshBots();
    else {
      setConnection('offline');
      retrySync();
    }
    return;
  }
  render(scroll, liveAfterId);
  if (context.openSettingsOnLoad) {
    context.openSettingsOnLoad = null;
    openSettingsFromMenu(context.id);
  }
}
const refreshBots = coalescedRefresh(async () => {
  try {
    const latest = await api('bots');
    const viewer = responseViewers.get(latest) ?? null;
    const previous = viewer === sidebarViewer ? readState : null;
    sidebarViewer = viewer;
    readState = new Map();
    // The first successful list sets a baseline for existing conversations in this tab.
    bots = latest.map(({ latestIncomingEventId: incoming, ...bot }) => {
      if (viewer !== null) {
        const readThrough = previous?.get(bot.id)?.readThrough ?? incoming;
        readState.set(bot.id, { incoming, readThrough });
      }
      return bot;
    });
    listLoaded = true;
    setConnection('ready');
    if (!bots.some((bot) => bot.id === view?.id)) selectLocation();
    render();
  } catch {
    setConnection('offline');
    retrySync();
  }
});
function readTimeline(context, read) {
  const next = (context.pageRead ?? Promise.resolve()).then(read, read);
  context.pageRead = next;
  return next;
}

async function loadOlder() {
  const context = timelineContext();
  if (!context?.olderBefore || context.loadingOlder) return;
  context.loadingOlder = true;
  if (context.peerId) context.olderFailed = false;
  renderTimeline();
  try {
    await readTimeline(context, async () => {
      if (context !== timelineContext() || !context.olderBefore) return;
      const cursor = context.olderBefore;
      const { events, olderBefore } = await conversationPage(context, cursor);
      if (context !== timelineContext() || context.olderBefore !== cursor) return;
      context.olderBefore = olderBefore;
      for (const event of events) context.events.set(event.id, event);
      renderTimeline('older');
    });
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
    mergeSessionTasks(context, page.sessionTasks);
    context.sessionsBefore = page.sessionsBefore;
  } catch { if (context === view) setNotice(context, 'Could not load earlier sessions.', true); }
  finally {
    context.loadingSessions = false;
    if (context === view) renderActivity();
  }
}

async function operate(context, task, operation, fields = {}) {
  const waitId = task.wait.id;
  await submitOperation(context, waitId, { taskId: task.id, waitId, operation,
    targetRevision: task.currentRevision ?? 0, targetGeneration: task.generation, ...fields });
}
async function submitOperation(context, key, payload) {
  if (context?.data?.canDecide !== true || pendingWaits.has(key)) return;
  const isOpen = () => view === context && (payload.itemKind
    ? detail?.kind === 'paused' && detail.messageId === payload.waitId
    : detail?.waitId === payload.waitId);
  pendingWaits.set(key, context.id);
  setNotice(context, 'Submitting action…');
  if (view?.id === context.id) render();
  let failed = false;
  try {
    await api(payload.itemKind ? 'inbox/decide' : 'operations', { botId: context.id, ...payload });
    setNotice(context, payload.operation === 'answer' ? 'Answer received.' : 'Action received.');
    if (isOpen()) closeDetail();
  } catch (error) {
    const message = errors[error.message] ?? 'Flock could not confirm the action. Check the conversation.';
    setNotice(context, message, true);
    if (isOpen()) showDetailError(message);
    failed = true;
  } finally {
    if ((failed || !liveUpdates.live) && view?.id === context.id) await Promise.all([view.refresh(), refreshBots()]);
    pendingWaits.delete(key);
    if (view?.id === context.id) render();
  }
}
async function submit(event) {
  event.preventDefault();
  if (view?.data?.canDecide !== true) return;
  const context = view;
  if (!context?.composer || context.dm || elements.send.disabled) return;
  const { composer } = context;
  const reply = composer.reply;
  const fields = { botId: context.id, text: composer.text };
  if (reply) fields.replyToMessageId = reply.messageId;
  const screenshots = screenshotsFor(composer)?.get(screenshotAnchor(reply));
  const attachments = screenshots?.images.map((image) => image.attachment) ?? [];
  let payload;
  sendingMessages.add(composer);
  setNotice(context, 'Sending message…');
  try {
    if (attachments.length) {
      const prepared = await screenshotMessage(fields, attachments, screenshots.pending);
      payload = prepared.payload;
      screenshots.pending = prepared.pending;
    } else payload = composer.prepareSend();
    await api('messages', payload);
    if (attachments.length) {
      composer.settleScreenshotSend(payload);
      clearScreenshots(composer, reply);
    } else composer.settleSend(payload, true);
    setNotice(context, 'Message received.');
    if (showingComposer(context.id)) elements.conversation.scrollTop = elements.conversation.scrollHeight;
    if (!liveUpdates.live) await Promise.all([refreshBots(), showingComposer(context.id) && view.refresh('bottom')]);
  } catch (error) {
    if (payload && error.status >= 400 && error.status < 500) {
      if (attachments.length) delete screenshots.pending;
      else composer.settleSend(payload, false);
    }
    const message = error instanceof ScreenshotError ? error.message : errors[error.message];
    setNotice(context, message ?? 'Flock could not confirm receipt. You can resend the same message.', true);
  }
  finally {
    sendingMessages.delete(composer);
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
elements.profile.addEventListener('click', () => openDetail({ kind: 'settings' }));
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
elements.conversation.addEventListener('scroll', updateJumpToLatest, { passive: true });
const timelineSize = new ResizeObserver(updateJumpToLatest);
timelineSize.observe(elements.conversation);
timelineSize.observe(elements.timeline);
elements['jump-to-latest'].addEventListener('pointerdown', (event) => {
  if (event.button === 0) event.preventDefault();
});
elements['jump-to-latest'].addEventListener('click', () => {
  const scroller = elements.conversation;
  scroller.scrollTop = scroller.scrollHeight;
  updateJumpToLatest();
});
elements['more-sessions'].addEventListener('click', loadSessions);
elements.composer.addEventListener('submit', submit);
elements['cancel-reply'].addEventListener('click', () => {
  view.composer.closeReply();
  renderComposer();
  elements.message.focus();
});
elements['attach-screenshots'].addEventListener('click', () => {
  if (elements['attach-screenshots'].getAttribute('aria-disabled') === 'true') return;
  pickerTarget = { context: view, composer: view.composer, reply: view.composer.reply };
  elements['screenshot-input'].click();
});
elements['screenshot-input'].addEventListener('change', (event) => {
  const files = Array.from(event.target.files);
  event.target.value = '';
  const target = pickerTarget;
  pickerTarget = null;
  if (target) void addScreenshots(target.context, target.composer, target.reply, files);
});
elements['screenshot-input'].addEventListener('cancel', () => { pickerTarget = null; });
elements.message.addEventListener('paste', (event) => {
  const files = Array.from(event.clipboardData?.files ?? []);
  if (!files.length || elements.message.readOnly) return;
  event.preventDefault();
  void addScreenshots(view, view.composer, view.composer.reply, files);
});
function updateDraft() {
  if (!view?.composer || view.dm || elements.message.readOnly) return;
  view.composer.edit(elements.message.value);
  fitMessage();
  notices.delete(view.id);
  const limit = composerLimit(view.composer.reply);
  if (byteLength(elements.message.value) > limit) setNotice(view, `Messages can be up to ${limit.toLocaleString('en')} bytes.`, true);
  else renderComposer();
}
elements.message.addEventListener('compositionstart', () => {
  if (!view?.composer || view.dm || elements.message.readOnly) return;
  composition = { composer: view.composer, anchor: view.composer.reply?.messageId };
  renderComposer();
});
elements.message.addEventListener('compositionend', () => {
  if (!composition) return;
  composition = null;
  if (elements.message.readOnly) renderComposer();
  else updateDraft();
});
elements.message.addEventListener('input', (event) => {
  if (composition || event.isComposing) fitMessage();
  else updateDraft();
});
// Safari reports the composition-committing Enter with keyCode 229 and isComposing false.
elements.composer.addEventListener('keydown', (event) => {
  if (composition || event.isComposing || event.keyCode === 229) return;
  if (event.target === elements.message) handleComposerEnter(event, elements.send);
  if (event.key === 'Escape' && !elements['reply-context'].hidden) {
    event.preventDefault();
    event.stopPropagation();
    elements['cancel-reply'].click();
  }
});
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

let syncRetry;
function retrySync() {
  if (!document.hidden) syncRetry ??= setTimeout(catchUp, 2500);
}

function catchUp() {
  clearTimeout(syncRetry);
  syncRetry = null;
  if (document.hidden) return;
  void refreshBots();
  void view?.refresh();
  void view?.dm?.refresh();
}

const liveUpdates = createLiveUpdates(catchUp, (change) => {
  if (document.hidden) return;
  if (change.list === true) void refreshBots();
  if (view && change.bot === view.id) void view.refresh(undefined, liveUpdates.live ? visibilityGeneration : null);
  const dm = view?.dm;
  if (dm && change.dm?.bot === dm.id && change.dm.peer === dm.peerId) void dm.refresh();
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
  catchUp();
}

elements.conversation.addEventListener('scroll', scheduleReadCheck, { passive: true });
new ResizeObserver(scheduleReadCheck).observe(elements.conversation);
document.addEventListener('close', scheduleReadCheck, true);
render();
resumeUpdates();
document.addEventListener('visibilitychange', () => {
  visibilityGeneration++;
  resumeUpdates();
});
window.addEventListener('pagehide', () => {
  visibilityGeneration++;
  liveUpdates.use(null);
});
window.addEventListener('pageshow', (event) => { if (event.persisted) resumeUpdates(); });
window.addEventListener('popstate', () => { if (listLoaded) selectLocation(); });

mountBoardInbox(api);
