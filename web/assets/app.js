import { handleComposerEnter, isComposerSuggestionKey } from '/composer-shortcuts.mjs';
import { accentColor, presenceMotion, showAvatar } from '/avatars.js';
import { actionLabel, approvalSummary } from '/action-labels.mjs';
import { refreshEventHistory, timelineMessages } from '/event-history.mjs';
import { coalescedRefresh, createLiveUpdates } from '/live-updates.js';
import { createBotEditor } from '/bot-editor.js';
import { messagePlacement, messageSide, peerMessagePlacement } from '/chat-grouping.mjs';
import { mountBoardInbox, pullRequestLink } from '/board-inbox.js';
import { ScreenshotError, screenshotMessage, stageScreenshots } from '/composer-screenshots.mjs';
import { createTimelineRows, placeRows } from '/timeline-rows.js';
import { replyArrivalId } from '/reply-arrival.mjs';
import { hasQuestionChoices, questionChoiceForm, questionReplyButton, updateQuestionChoiceForm } from '/question-choices.js';
import { createComposerDraftStore } from '/composer-drafts.mjs';
import { mountConversationFind } from '/conversation-find.mjs';
import { createRecurringRequests } from '/recurring-requests.js';

const byId = (id) => document.getElementById(id);
const elements = Object.fromEntries([
  'navigation', 'pinned', 'bot-list', 'wait-list', 'wait-count', 'session-list', 'more-sessions',
  'connection', 'connection-text', 'new-bot-button', 'open-navigation', 'close-navigation', 'profile-avatar', 'profile-avatar-button',
  'profile-name', 'profile-status', 'profile-identity', 'profile-toolbar', 'profile-placeholder', 'conversation', 'older',
  'timeline', 'conversation-source-error', 'jump-to-latest', 'composer-container', 'composer', 'reply-context', 'reply-question', 'cancel-reply',
  'decision-reply-context', 'decision-reply-label', 'decision-reply-target',
  'reply-label', 'compose-label', 'message', 'composer-ghost', 'send', 'notice', 'view-only-notice', 'detail', 'detail-title', 'detail-body', 'close-detail',
  'attach-screenshots', 'screenshot-input', 'screenshot-previews',
  'bot-editor', 'theme-switch', 'bot-activity', 'activity-scope-avatar', 'activity-scope-name',
  'peer-identity', 'peer-heading', 'peer-faces', 'peer-parent-link', 'peer-name-first', 'peer-name-second',
  'peer-back', 'peer-back-label', 'peer-picker', 'peer-status', 'sidebar-resizer',
].map((id) => [id, byId(id)]));
const updateTimelineRows = createTimelineRows(elements.timeline, elements.conversation);
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
const motionAnimations = new WeakMap();
const motionEaseOut = 'cubic-bezier(0.23, 1, 0.32, 1)';
const motionSpring = 'cubic-bezier(0.22, 1.2, 0.36, 1)';
function cancelMotion(el) {
  const prev = el && motionAnimations.get(el);
  if (!prev) return;
  motionAnimations.delete(el);
  prev.cancel();
}
function runMotion(el, keyframes, options) {
  if (!el?.animate || reducedMotion.matches) return null;
  cancelMotion(el);
  const animation = el.animate(keyframes, options);
  motionAnimations.set(el, animation);
  const clear = () => {
    if (motionAnimations.get(el) === animation) motionAnimations.delete(el);
  };
  animation.addEventListener('finish', clear, { once: true });
  animation.addEventListener('cancel', clear, { once: true });
  return animation;
}
function animateLiveReply(id) {
  runMotion(byId(`event-${id}`)?.querySelector('.bubble'), [
    { opacity: .5, transform: 'translateY(8px) scale(.975)' },
    { opacity: 1, transform: 'none' },
  ], { duration: 200, easing: motionSpring });
}
function animateSentMessage(id) {
  runMotion(byId(`event-${id}`)?.querySelector('.bubble'), [
    { opacity: 0, transform: 'translateY(10px) scale(.96)' },
    { opacity: 1, transform: 'none' },
  ], { duration: 210, easing: motionSpring });
}
const workspace = document.querySelector('.workspace');
const conversationActions = byId('conversation-actions');
const conversationActionsTrigger = conversationActions.querySelector('summary');
const narrow = matchMedia('(max-width: 700px)');
const noHover = matchMedia('(hover: none)');
const clock = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
const dayFormat = new Intl.DateTimeFormat('en', { year: 'numeric', month: 'long', day: 'numeric' });
const fullTime = new Intl.DateTimeFormat('en', { dateStyle: 'medium', timeStyle: 'short', hourCycle: 'h23' });
const shortDay = new Intl.DateTimeFormat('en', { month: 'short', day: 'numeric' });
const byteLength = (text) => new TextEncoder().encode(text).length;
const sessionEndLabels = {
  completed: 'Complete', cancelled: 'Cancelled', failed: 'Failed',
  interrupted: 'Interrupted', rolled_back: 'Rolled back', ended: 'Ended',
};
const stateLabels = {
  ...sessionEndLabels,
  queued: 'Queued', deciding: 'Evaluating', sending: 'Sending', running: 'Working', waiting: 'Waiting for you',
  cancelling: 'Cancelling', stopping: 'Confirming stop',
  idle: 'Idle', thinking: 'Thinking', working: 'Working', peer_waiting: 'Waiting for another Bot',
};
// Wait copy names the open Bot (`$BOT`) rather than a generic "the Bot". An optional third entry is the
// Bot's own first-person wording for its conversation bubble.
const waitLabels = {
  user_question: ['Question from $BOT', '$BOT is waiting for your answer.', 'I’m waiting for your answer.'],
  budget: ['Approval for one more run', 'This request used all of its runs. Approving lets $BOT start one more run, which can use more model and T3 usage.',
    'This request used all of its runs. If you approve, I can start one more run, which can use more model and T3 usage.'],
  policy_approval: ['Approval needed', '$BOT is waiting for your approval before it continues.', 'I need your approval before I continue.'],
  model_budget: ['Approval for another model call', 'This request reached its model call limit. You can approve one more model call.'],
  model_failure: ['$BOT got stuck', '$BOT couldn’t choose its next step after several attempts. Try again to continue.',
    'I couldn’t choose my next step after several attempts. Try again to continue.'],
  t3_input: ['Action in T3', 'Open the Session in T3 to approve or answer.'],
  deadline: ['Deadline extension', 'This request reached its deadline. You can set a new one.'],
  unknown_delivery: ['Message to T3 unconfirmed', 'Flock could not confirm whether T3 received the last message, so this work is paused. Check again asks T3 without resending anything.'],
  auth: ['T3 sign-in needed', 'This work is paused because the T3 sign-in expired. Sign in to T3 again, then check again.'],
  settings: ['T3 settings', 'This work is paused because of the T3 connection or execution settings. Fix them, then check again.'],
};
// Name what the person will do in the dialog; the ellipsis says it opens a step before anything happens.
const pendingActionLabels = {
  budget: 'Approve one more run…', model_budget: 'Approve another model call…', model_failure: 'Try again…',
  deadline: 'Extend deadline…', t3_input: 'Open in T3…', unknown_delivery: 'Resolve…', auth: 'Resolve…', settings: 'Resolve…',
};
const closedLabels = {
  answer: 'Answered', run_finished: 'Session ended', cancel: 'Stop requested',
  session_question: 'The session updated the question', withdrawn: 'Question withdrawn', request_changed: 'Request changed',
  approve: 'Approved', extend: 'Deadline extended', reconcile: 'Check requested', supply: 'Secret saved',
  allow_once: 'Allowed once', always_allow: 'Always allowed', deny: 'Denied', expired: 'Approval expired',
  stale: 'No longer needed', delivery_confirmed: 'Delivery confirmed', connection_restored: 'Connection restored',
  superseded: 'Replaced', superseded_instruction: 'Replaced by a newer instruction',
};
// Confirms what the response will do, so the person isn't left with a bare "Done".
const decisionConfirmations = {
  answer: (name) => `Answer recorded for ${name}.`,
  allow_once: () => 'Allowed once.',
  always_allow: () => 'Allowed and saved as a rule.',
  allow_next_time: () => 'Allow rule saved.',
  deny: () => 'Denied.',
  approve: () => 'Approved.',
  request_changes: (name) => `Change request recorded for ${name}.`,
  extend: () => 'Deadline extended.',
  reconcile: () => 'Check requested.',
  supply: () => 'Secret saved.',
  link_pr: () => 'Pull request linked.',
  reject_pr: () => 'Pull request link rejected.',
};
const errors = {
  invalid_text: 'Check the message and its length.', invalid_deadline: 'Set a deadline in the future.',
  message_conflict: 'A message was already recorded for this submission. Check the conversation.',
  operation_conflict: 'Flock already processed this action or the pending request changed.',
  request_changed: 'The request changed. Review its current details before acting again.',
  already_closed: 'This request was already answered or closed.',
  access_denied: 'You do not have permission to respond to this request.',
  bot_not_found: 'Bot not found.',
  invalid_peer: 'Choose another Bot for this DM.',
  peer_not_allowed: 'This Bot DM is unavailable.',
  peer_not_found: 'This Bot DM is unavailable.',
  peer_message_not_found: 'This message is unavailable.',
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
const openBotName = () => view?.data?.bot?.name ?? bots.find((bot) => bot.id === view?.id)?.name ?? 'The Bot';
const withBotName = (text) => text?.replaceAll('$BOT', openBotName);
function waitLabel(reason) { return withBotName(waitLabels[reason]?.[0]) ?? 'Waiting for you'; }
function waitText(wait, { spokenByBot = false } = {}) {
  const labels = waitLabels[wait.reason];
  const fallback = withBotName((spokenByBot && labels?.[2]) || labels?.[1]);
  if (wait.reason === 'policy_approval') return fallback;
  return wait.text || fallback || 'Refresh the pending request.';
}
function changed(key, ...values) {
  const signature = JSON.stringify(values);
  if (signatures.get(key) === signature) return false;
  signatures.set(key, signature);
  return true;
}
const responseViewers = new WeakMap();
async function api(path, body, { method = body ? 'POST' : 'GET', signal } = {}) {
  const headers = new Headers();
  if (method !== 'GET') headers.set('X-Flock-Action', '1');
  if (body) headers.set('Content-Type', 'application/json');
  const response = await fetch(`/api/${path}`, {
    method, headers, credentials: 'same-origin', signal, body: body ? JSON.stringify(body) : undefined,
  });
  const data = await response.json();
  if (!response.ok) throw Object.assign(new Error(data.error), { status: response.status });
  const viewer = response.headers.get('X-Flock-Viewer');
  if (viewer !== null) responseViewers.set(data, viewer);
  return data;
}
const botQuery = (path, bot, before) => `${path}?${new URLSearchParams(before ? { bot, before } : { bot })}`;
function peerQuery({ id: bot, peerId: peer }, before, anchor) {
  const params = new URLSearchParams({ bot, peer });
  if (before) params.set('before', before);
  else if (anchor) params.set('anchor', anchor);
  return `peer-conversation?${params}`;
}
const timelineContext = () => view?.dm ?? view;
const timelineKey = (botId, peerId = null) => JSON.stringify([botId, peerId]);
const showingComposer = (bot) => view?.id === bot && !view?.dm;

function setConnection(state) {
  if (connection === state) return;
  connection = state;
  elements.connection.dataset.state = state;
  elements['connection-text'].textContent = state === 'offline' ? 'Can’t reach Flock. Reconnecting…' : '';
  renderHeader();
}
let themeSwitchTimer = 0;
function resolvedTheme(mode) {
  return mode === 'system'
    ? (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light')
    : mode;
}
function syncThemeIndicator(animate = false) {
  const group = elements['theme-switch'];
  const active = group?.querySelector('button[aria-pressed="true"]');
  if (!group || !active || !active.offsetWidth) return;
  const x = `${active.offsetLeft}px`;
  const w = `${active.offsetWidth}px`;
  const wasReady = group.classList.contains('seg-ready');
  if (wasReady && group.style.getPropertyValue('--seg-x') === x && group.style.getPropertyValue('--seg-w') === w) {
    return;
  }
  if (animate && wasReady && !reducedMotion.matches) {
    if (!group.classList.contains('seg-animated')) {
      group.classList.add('seg-animated');
      void group.offsetWidth;
    }
  } else {
    group.classList.remove('seg-animated');
  }
  group.style.setProperty('--seg-x', x);
  group.style.setProperty('--seg-w', w);
  group.classList.add('seg-ready');
}
function renderTheme(mode, animate = false) {
  for (const option of elements['theme-switch'].querySelectorAll('button')) {
    option.setAttribute('aria-pressed', option.dataset.themeMode === mode);
  }
  syncThemeIndicator(animate);
}
function setTheme(mode) {
  const group = elements['theme-switch'];
  const active = group.querySelector('button[aria-pressed="true"]');
  const prevMode = active?.dataset.themeMode ?? 'system';
  const prevResolved = resolvedTheme(prevMode);
  const nextResolved = resolvedTheme(mode);
  save('flock-theme', mode);
  if (!reducedMotion.matches) {
    if (group.classList.contains('seg-ready')) group.classList.add('seg-animated');
    if (prevResolved !== nextResolved) {
      const root = document.documentElement;
      root.classList.add('theme-switching');
      clearTimeout(themeSwitchTimer);
      themeSwitchTimer = setTimeout(() => {
        root.classList.remove('theme-switching');
        themeSwitchTimer = 0;
      }, 260);
    }
    void document.documentElement.offsetWidth;
  }
  renderTheme(mode, true);
  document.dispatchEvent(new CustomEvent('flock-theme', { detail: mode }));
}
const mainElement = byId('main');
const navBackdrop = byId('nav-backdrop');
function openNavigation(open, focus = true) {
  const expanded = open && narrow.matches;
  const navigationVisibilityChanged = workspace.classList.contains('nav-open') !== expanded;
  workspace.classList.toggle('nav-open', expanded);
  if (mainElement) mainElement.inert = expanded;
  elements['open-navigation'].setAttribute('aria-expanded', expanded);
  if (focus && narrow.matches && navigationVisibilityChanged) elements[expanded ? 'close-navigation' : 'open-navigation'].focus();
}
function closeConversationActions(focus = false) {
  if (!conversationActions.open) return;
  conversationActions.open = false;
  if (focus) conversationActionsTrigger.focus({ preventScroll: true });
}
function conversationUrl(botId, peerId, anchor) {
  const url = new URL(location.href);
  if (botId) url.searchParams.set('bot', botId);
  else url.searchParams.delete('bot');
  if (peerId) url.searchParams.set('peer', peerId);
  else url.searchParams.delete('peer');
  if (peerId && anchor) url.searchParams.set('anchor', anchor);
  else url.searchParams.delete('anchor');
  return url;
}
function animateTimelineContent(kind) {
  if (reducedMotion.matches) return;
  const inner = elements.conversation.querySelector('.conversation-inner');
  cancelMotion(inner);
  cancelMotion(elements.timeline);
  if (kind === 'dm-enter') {
    runMotion(inner, [
      { opacity: 0, transform: 'translateX(18px)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 220, easing: motionEaseOut });
  } else if (kind === 'dm-exit') {
    runMotion(inner, [
      { opacity: 0, transform: 'translateX(-18px)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 220, easing: motionEaseOut });
  } else if (kind === 'bot-switch') {
    runMotion(elements.timeline, [
      { opacity: .15, transform: 'translateY(8px)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 210, easing: motionEaseOut });
  }
}
function animateViewTransition(kind) {
  const inner = elements.conversation.querySelector('.conversation-inner');
  const profileHeading = byId('profile-heading');
  for (const target of [inner, elements.timeline, elements['peer-faces'], elements['peer-heading'],
      elements['peer-back'], elements['profile-avatar-button'], profileHeading]) {
    cancelMotion(target);
  }
  if (reducedMotion.matches) return;
  const context = timelineContext();
  if (elements.timeline?.childElementCount > 0) {
    animateTimelineContent(kind);
    if (context) context.pendingViewMotion = null;
  } else if (context) {
    context.pendingViewMotion = kind;
  }
  if (kind === 'dm-enter') {
    runMotion(elements['peer-faces'], [
      { opacity: 0, transform: 'translateX(12px) scale(.92)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 220, easing: motionSpring });
    runMotion(elements['peer-heading'], [
      { opacity: 0, transform: 'translateX(14px) scale(.96)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 220, easing: motionEaseOut });
    runMotion(elements['peer-back'], [
      { opacity: 0, transform: 'translateX(-10px)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 200, easing: motionEaseOut });
  } else if (kind === 'dm-exit') {
    runMotion(elements['profile-avatar-button'], [
      { opacity: 0, transform: 'translateX(-10px) scale(.92)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 220, easing: motionSpring });
    runMotion(profileHeading, [
      { opacity: 0, transform: 'translateX(-12px) scale(.96)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 220, easing: motionEaseOut });
  } else if (kind === 'bot-switch') {
    runMotion(elements['profile-avatar-button'], [
      { opacity: .6, transform: 'scale(.88) translateY(3px)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 230, easing: motionSpring });
    runMotion(profileHeading, [
      { opacity: .5, transform: 'translateY(4px) scale(.96)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 220, easing: motionSpring });
  }
}
function selectBot(botId, peerId = null, { updateHistory = true, anchor = 0, fromPeerLink = false } = {}) {
  menu = null;
  closeConversationActions();
  openNavigation(false);
  if (view?.id === botId && (view?.dm?.peerId ?? null) === peerId && (view?.dm?.anchor ?? 0) === anchor) {
    if (view.source || view.pendingSource) backToLatest();
    else renderBots();
    return;
  }
  const prevBotId = view?.id ?? null;
  const wasDm = !!view?.dm;
  const isDm = !!peerId;
  resetConversationJump(view);
  rememberReadingPosition(fromPeerLink && !view?.dm && view?.id === botId);
  closeDetail();
  const existing = view?.id === botId ? view : null;
  view = botId ? { id: botId, data: null, events: new Map(), olderBefore: 0, loadedSessions: new Map(),
    sessionsBefore: 0, ...existing, source: null, pendingSource: null, loadingOlder: false, loadingSessions: false,
    dm: peerId ? { id: botId, peerId, anchor, fromPeerLink, events: new Map(), olderBefore: 0 } : null } : null;
  const url = conversationUrl(botId, peerId, anchor);
  if (url.href !== location.href) {
    if (updateHistory) history.pushState(fromPeerLink ? { peerAnchor: anchor } : null, '', url);
    else history.replaceState(null, '', url);
  }
  save('flock-selected-bot', botId);
  render('bottom');
  if (!wasDm && isDm) animateViewTransition('dm-enter');
  else if (wasDm && !isDm) animateViewTransition('dm-exit');
  else if (prevBotId && prevBotId !== botId && !isDm) animateViewTransition('bot-switch');
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
  const wantsSettings = params.get('settings') === '1';
  const peer = params.get('peer');
  const peerId = !wantsSettings && bot && peer && peer !== bot.id ? peer : null;
  const requestedAnchor = params.get('anchor');
  const id = /^\d+$/.test(requestedAnchor ?? '') ? Number(requestedAnchor) : 0;
  const anchor = peerId && Number.isSafeInteger(id) ? id : 0;
  const preferred = saved('flock-selected-bot', null);
  const selectedBot = bot?.id ?? bots.find((entry) => entry.id === preferred)?.id ?? bots[0]?.id ?? null;
  if (!peerId && view?.dm && view.id === selectedBot) leavePeerConversation(false);
  else selectBot(selectedBot, peerId, { updateHistory: false, anchor, fromPeerLink: !!anchor && history.state?.peerAnchor === anchor });
  if (wantsSettings) {
    const url = conversationUrl(selectedBot);
    url.searchParams.delete('settings');
    history.replaceState(history.state, '', url);
    if (bot && view?.id === bot.id) openBotSettings(bot.id);
  }
}
function peerLink(botId, peerId, label, className, anchor = 0) {
  const link = node('a', className, label);
  link.href = conversationUrl(botId, peerId, anchor);
  link.addEventListener('click', (event) => {
    if (event.button || event.ctrlKey || event.metaKey || event.altKey || event.shiftKey) return;
    event.preventDefault();
    selectBot(botId, peerId, { anchor, fromPeerLink: !!anchor });
    elements['peer-heading'].focus({ preventScroll: true });
  });
  return link;
}

const sidebarIcons = {
  more: '<svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><circle cx="6" cy="12" r="1.6"/><circle cx="12" cy="12" r="1.6"/><circle cx="18" cy="12" r="1.6"/></svg>',
  pin: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 4h6l-1 5 3 3v2H7v-2l3-3-1-5Z"/><path d="M12 14v6"/></svg>',
  inbox: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 13.5 6.2 6a2 2 0 0 1 1.9-1.5h7.8A2 2 0 0 1 17.8 6l2.2 7.5V18a1.5 1.5 0 0 1-1.5 1.5h-13A1.5 1.5 0 0 1 4 18v-4.5Z"/><path d="M4 13.5h4.5l1 2h5l1-2H20"/></svg>',
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
    ['History', sidebarIcons.inbox, () => {
      selectBot(bot.id);
      void decisions.openHistory(sidebar.querySelector(`[data-focus="${CSS.escape(`menu:${bot.id}`)}"]`));
    }],
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
  const select = button(null, '', () => {
    decisions.close({ focus: false });
    selectBot(botId);
  });
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
  name.append(nameText);
  const preview = node('span', 'bot-preview');
  const dots = node('span', 'working-dots');
  dots.setAttribute('aria-hidden', 'true');
  dots.append(node('i'), node('i'), node('i'));
  const previewText = document.createTextNode('');
  preview.append(presenceDot, dots, previewText);
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
  const decisionCount = button(null, 'decision-count', () => {
    selectBot(botId);
    decisions.open(decisionCount);
  });
  decisionCount.dataset.focus = `decisions:${botId}`;
  decisionCount.setAttribute('aria-controls', 'decision-panel');
  row.append(select, tooltip, decisionCount, toggle);
  row.addEventListener('contextmenu', (event) => {
    event.preventDefault();
    setMenu(botId, 'first');
  });

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
      const count = decisions.count(botId);
      decisionCount.hidden = !count;
      decisionCount.textContent = count;
      decisionCount.setAttribute('aria-label', `${bot.name}: ${count} ${count === 1 ? 'item needs' : 'items need'} you`);

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
      // A Bot blocked on the person says so in place of its last message, so the row reads at a glance.
      const waiting = bot.presence === 'waiting';
      const previewLabel = waiting ? 'Waiting for you' : bot.preview || '';
      if (previewText.data !== previewLabel) previewText.data = previewLabel;
      preview.classList.toggle('waiting', waiting);
      if (waiting) preview.setAttribute('aria-hidden', 'true');
      else preview.removeAttribute('aria-hidden');
      const hasPresence = activePresence.has(bot.presence);
      presenceDot.hidden = !waiting;
      dots.hidden = !busyPresence.has(bot.presence);
      preview.hidden = !previewLabel && dots.hidden;
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

let emptyBotRow;
function newBotRow() {
  if (emptyBotRow) return emptyBotRow;
  emptyBotRow = node('div', 'bot-row new-bot-row');
  const select = button(null, 'bot-select', () => openNewBot(emptyBotRow.querySelector('button')));
  const mark = node('span', 'new-bot-mark');
  mark.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>';
  const text = node('span', 'bot-text');
  text.append(node('span', 'bot-name', 'New Bot'), node('span', 'bot-preview', 'No Bots yet'));
  select.append(mark, text);
  emptyBotRow.append(select);
  return emptyBotRow;
}

function draftRow(draft) {
  const row = node('div', 'bot-row draft');
  row.setAttribute('aria-hidden', 'true');
  const select = node('span', 'bot-select');
  const name = node('span', 'bot-name');
  name.append(node('span', 'bot-name-text', draft.name));
  const text = node('span', 'bot-text');
  text.append(name);
  select.append(avatar(draft, 'bot-avatar'), text);
  row.append(select);
  return row;
}

const rowRevealTimers = new WeakMap();
function triggerRowReveal(container) {
  if (reducedMotion.matches) return;
  clearTimeout(rowRevealTimers.get(container));
  container.classList.remove('rows-reveal');
  void container.offsetWidth;
  container.classList.add('rows-reveal');
  rowRevealTimers.set(container, setTimeout(() => {
    container.classList.remove('rows-reveal');
    rowRevealTimers.delete(container);
  }, 400));
}
function clearRowReveal(container) {
  clearTimeout(rowRevealTimers.get(container));
  rowRevealTimers.delete(container);
  container.classList.remove('rows-reveal');
}

let previousPinsKey = null;
let previousListLoaded = false;
function renderBots() {
  const unreadIds = bots.filter(hasUnread).map((bot) => bot.id);
  if (!changed('bots', bots, [...pins], menu, view?.id, listLoaded, botDraft, unreadIds,
    bots.map(({ id }) => decisions.count(id)))) return;
  const revealedFromSkeleton = !previousListLoaded && listLoaded;
  previousListLoaded = listLoaded;
  const pinsKey = JSON.stringify([...pins]);
  const animateFlip = previousPinsKey !== null && previousPinsKey !== pinsKey && !reducedMotion.matches;
  previousPinsKey = pinsKey;
  if (animateFlip || menu) {
    clearRowReveal(elements['bot-list']);
    clearRowReveal(elements.pinned);
  }
  const beforeRects = new Map();
  if (animateFlip) {
    for (const entry of sidebarEntries.values()) {
      if (entry.row.isConnected) beforeRects.set(entry.row, entry.row.getBoundingClientRect());
    }
    if (!elements['bot-activity'].hidden) {
      beforeRects.set(elements['bot-activity'], elements['bot-activity'].getBoundingClientRect());
    }
  }
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
  else if (!bots.length && !botDraft) rows.push(newBotRow());
  placeRows(list, rows);
  pinned.forEach((bot, index) => sidebarEntries.get(bot.id).update(bot, index));
  unpinned.forEach((bot) => sidebarEntries.get(bot.id).update(bot, -1));
  if (revealedFromSkeleton) {
    if (unpinned.length) triggerRowReveal(list);
    if (pinned.length) triggerRowReveal(elements.pinned);
  }
  if (animateFlip) {
    for (const [el, before] of beforeRects) {
      if (!el.isConnected || el.hidden) continue;
      const after = el.getBoundingClientRect();
      if (!after.width || !after.height) continue;
      const dx = before.left - after.left;
      const dy = before.top - after.top;
      if (Math.abs(dx) > 1 || Math.abs(dy) > 1) {
        el.animate?.([
          { transform: `translate(${dx}px, ${dy}px)` },
          { transform: 'none' },
        ], { duration: 230, easing: 'cubic-bezier(.2, .9, .25, 1)' });
      }
    }
  }
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
        document.hidden || decisions.isOpen() || document.querySelector('dialog[open]')) return;
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

const presenceLabels = { idle: 'Idle', thinking: 'Thinking', working: 'Working', waiting: 'Waiting for you', peer_waiting: 'Waiting for another Bot', stopping: 'Confirming stop', queued: 'Queued' };
const participant = (id) => bots.find((bot) => bot.id === id) ?? { id, name: id };
const botPeers = (id) => participant(id).peers ?? [];

function renderHeader() {
  const bot = view?.data?.bot ?? bots.find((item) => item.id === view?.id);
  const dm = view?.dm;
  conversationFind.setContext(view?.data && !dm ? view : null);
  const profile = elements['profile-avatar-button'];
  const identity = elements['profile-identity'];
  identity.hidden = !bot || !!dm;
  elements['profile-toolbar'].hidden = !bot;
  // The first-run empty state carries its own heading; a bare "Conversation" title would only repeat it.
  elements['profile-placeholder'].hidden = !!bot || !!dm || (listLoaded && !bots.length);
  elements['bot-activity'].hidden = !bot || !!dm;
  elements['peer-picker'].hidden = !view?.data || !!dm || !botPeers(bot.id).length;
  elements['peer-back'].hidden = !dm;
  elements['peer-identity'].hidden = !dm;
  if (dm) {
    const owner = participant(dm.id);
    const peer = participant(dm.peerId);
    const backLabel = `Back to ${owner.name}`;
    elements['peer-back-label'].textContent = backLabel;
    elements['peer-back'].setAttribute('aria-label', backLabel);
    elements['peer-back'].title = `${backLabel} (Esc)`;
    if (changed('peer-heading', [owner.id, owner.name, owner.avatar, peer.id, peer.name])) {
      elements['peer-faces'].replaceChildren(avatar(owner, 'avatar-profile'));
      elements['peer-name-first'].textContent = owner.name;
      elements['peer-name-second'].textContent = peer.name;
      elements['peer-heading'].title = `${owner.name} › ${peer.name}`;
    }
  }
  const nextScopeKey = bot ? JSON.stringify([bot.id, bot.name, bot.avatar]) : '';
  if (nextScopeKey !== scopeKey) {
    scopeAvatar?.destroy();
    scopeAvatar = bot ? showAvatar(elements['activity-scope-avatar'], bot, { still: false }) : null;
    if (bot) elements['activity-scope-name'].textContent = bot.name;
    // Accent tokens follow the open Bot, including its Bot-to-Bot DMs; the empty state keeps Flock purple.
    if (bot) document.documentElement.style.setProperty('--bot-accent', accentColor(bot));
    else document.documentElement.style.removeProperty('--bot-accent');
  }
  scopeKey = nextScopeKey;
  if (!bot || dm) {
    profileAvatar?.destroy();
    profileAvatar = null;
    profileKey = '';
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
  } else if (profileMotion !== motion) {
    profileAvatar?.update(look);
  }
  profileMotion = motion;
  const offline = connection === 'offline';
  const status = offline ? 'Offline' : presenceLabels[presence] ?? presence;
  const showStatus = offline || (presence !== 'idle' && !(presence === 'waiting' && decisions.count(bot.id)));
  const settingsLabel = `${bot.name} settings`;
  const name = elements['profile-name'];
  identity.dataset.state = offline ? 'offline' : presence;
  profile.setAttribute('aria-label', settingsLabel);
  name.textContent = bot.name;
  name.title = settingsLabel;
  elements['profile-status'].textContent = showStatus ? status : '';
  elements['profile-status'].hidden = !showStatus;
  if (showStatus) profile.setAttribute('aria-describedby', 'profile-status');
  else profile.removeAttribute('aria-describedby');
}

function activityRow(tag, className, title, status, statusClass) {
  const row = node(tag, `activity ${className}`);
  const heading = node('span', 'activity-title', title);
  const line = node('span', `activity-status ${statusClass}`.trim());
  line.append(node('span', 'activity-mark'), node('span', null, status));
  row.append(heading, line);
  return row;
}
function sessionRow(request, session, status, tone, startedAt) {
  const link = runHref(session);
  const row = activityRow(link ? 'a' : 'div', 'session-row', request, status, tone);
  row.title = Number.isFinite(startedAt) ? `${request} · Started ${fullTime.format(startedAt)}` : request;
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
const workflowStatusLabels = {
  pending: 'Not started', running: 'In progress', completed: 'Done', failed: 'Failed',
  needs_verification: 'Needs another check', stop_requested: 'Stopping', stopped: 'Stopped',
};
const stageStatusLabels = {
  pending: 'Waiting', ready: 'Starting', running: 'Working', completed: 'Done', failed: 'Failed',
  stopped: 'Stopped', stop_requested: 'Stopping', unreachable: 'Not responding',
};
const stageTone = (status) => ({ completed: 'done', running: 'live', ready: 'live', failed: 'failed', unreachable: 'failed', stopped: 'stopped' })[status] ?? 'waiting';
const workflowTone = (status) => ({ running: 'live', failed: 'failed', needs_verification: 'waiting', stop_requested: 'waiting' })[status] ?? '';
const humanStatus = (labels, status) => labels[status] ?? (status.charAt(0).toUpperCase() + status.slice(1)).replaceAll('_', ' ');

function workflowInspectors(data, open) {
  const sections = [];
  const section = (key, title, status, tone) => {
    const item = node('details', 'activity-details');
    item.dataset.inspector = key;
    item.open = open.has(key);
    const summary = node('summary', 'activity');
    const line = node('span', `activity-status ${tone}`.trim());
    line.append(node('span', 'activity-mark'), node('span', null, status));
    summary.append(node('span', 'activity-title', title), line);
    item.append(summary);
    sections.push(item);
    return item;
  };
  for (const workflow of data?.workflows ?? []) {
    const stages = workflow.stages ?? [];
    const done = stages.filter((stage) => stage.status === 'completed' && !stage.outdated).length;
    const status = [humanStatus(workflowStatusLabels, workflow.status), stages.length ? `${done} of ${stages.length} steps` : '']
      .filter(Boolean).join(' · ');
    const item = section(`workflow-${workflow.workflowId}`, workflow.request, status, workflowTone(workflow.status));
    const list = node('ol', 'workflow-stages');
    for (const stage of stages) {
      const verdict = stage.currentVerdict;
      const row = node('li', 'workflow-stage');
      row.dataset.tone = stage.outdated ? 'waiting' : stageTone(stage.status);
      const notes = [];
      if (verdict) notes.push(verdict === 'pass' ? ['Check passed', 'pass'] : ['Check failed', 'fail']);
      if (stage.outdated) notes.push(['Needs a rerun after earlier changes', 'warn']);
      if (stage.deadlineExceeded) notes.push(['Taking longer than planned', 'warn']);
      if (stage.stopRequested && stage.status === 'unreachable') notes.push(['Stop requested', 'warn']);
      const text = node('span', 'workflow-stage-text');
      const head = node('span', 'workflow-stage-head');
      head.append(node('span', 'workflow-stage-name', stage.name), node('span', 'workflow-stage-status', humanStatus(stageStatusLabels, stage.status)));
      text.append(head);
      for (const [note, tone] of notes) {
        const line = node('span', 'workflow-stage-note', note);
        line.dataset.tone = tone;
        text.append(line);
      }
      const link = linkToRun(stage.runRef, 'Open');
      if (link) head.append(link);
      row.append(node('span', 'workflow-stage-mark'), text);
      row.title = [stage.inputArtifactVersion && `Input ${stage.inputArtifactVersion}`, stage.outputArtifactVersion && `Output ${stage.outputArtifactVersion}`]
        .filter(Boolean).join('\n');
      list.append(row);
    }
    item.append(list);
  }
  for (const summary of data?.compactions ?? []) {
    const item = section(`summary-${summary.compactionId}`, 'Earlier conversation', `Summary of ${summary.originalCount} messages`, '');
    item.append(node('p', 'activity-details-body', summary.summary));
  }
  return sections;
}
const decisionKindLabels = {
  user_question: 'Question for you',
  pr_review: 'Review requested',
  pr_reconciliation: 'Confirm the pull request',
  extra_run: 'Approve one more run',
  deadline: 'Deadline reached',
  model_budget: 'Approve another model call',
  conversation_model_budget: 'Approve another model call',
  unknown_delivery: 'Message to T3 unconfirmed',
  protected_input: 'Secret needed',
  policy_approval: 'Approval needed',
};
let activityBotId = null;
let activitySeen = false;
let activityReveal = null;
let previousWaitsLoading = false;
let previousSessionsLoading = false;
function renderActivity() {
  const data = view?.data;
  const loading = !!view && !data;
  const pending = view?.id ? decisions.itemsFor(view.id) : [];
  if (changed('waits', view?.id, loading, pending)) {
    const revealedWaits = previousWaitsLoading && !loading;
    previousWaitsLoading = loading;
    elements['wait-count'].textContent = pending.length;
    elements['wait-count'].hidden = !pending.length;
    elements['wait-list'].setAttribute('aria-busy', loading);
    const rows = pending.map((item) => {
      const status = decisionKindLabels[item.itemKind] ?? 'Needs you';
      const title = item.itemKind === 'policy_approval' && item.action
        ? [actionLabel(item.action), item.target].filter(Boolean).join(' · ') : item.requestTitle || item.request;
      const row = activityRow('button', 'wait-row', title, status, 'waiting');
      row.type = 'button';
      row.title = item.itemKind === 'policy_approval' ? approvalSummary(item.prompt, item) || title : item.prompt || title;
      row.addEventListener('click', () => {
        openNavigation(false);
        decisions.openItem(item, row);
      });
      return row;
    });
    elements['wait-list'].replaceChildren(...rows);
    if (loading) elements['wait-list'].append(...skeletonRows(1));
    else if (revealedWaits && rows.length) triggerRowReveal(elements['wait-list']);
    elements['wait-list'].closest('section').hidden = !loading && !rows.length;
  }
  const groups = [...(view?.loadedSessions.values() ?? [])].filter(task => task.id >= view.sessionsBefore);
  const active = data?.activeSessions ?? [];
  const more = elements['more-sessions'];
  more.hidden = !view?.sessionsBefore;
  more.disabled = !!view?.loadingSessions;
  if (changed('sessions', view?.id, loading, groups, active, data?.workflows, data?.compactions, dayFormat.format(Date.now()))) {
    const revealedSessions = previousSessionsLoading && !loading;
    previousSessionsLoading = loading;
    const key = (taskId, run) => JSON.stringify([taskId, run.environmentId, run.threadId]);
    const activeKeys = new Set();
    const live = [];
    for (const run of active) {
      const identity = key(run.taskId, run);
      if (activeKeys.has(identity)) continue;
      activeKeys.add(identity);
      live.push(sessionRow(run.request, run, stateLabels[run.state] ?? run.state,
        run.state === 'waiting' ? 'waiting' : 'live', run.createdAt ? run.createdAt * 1000 : undefined));
    }
    const ended = [];
    for (const task of groups.sort((a, b) => b.id - a.id)) {
      for (const session of task.sessions) {
        if (activeKeys.has(key(task.id, session))) continue;
        const startedAt = Date.parse(session.createdAt);
        const started = Number.isFinite(startedAt) ? startedLabel(startedAt) : '';
        const quiet = session.state === 'completed' && !!started;
        const end = Object.hasOwn(sessionEndLabels, session.state) ? sessionEndLabels[session.state] : sessionEndLabels.ended;
        const status = quiet ? started : [end, started].filter(Boolean).join(' · ');
        const tone = quiet ? 'quiet' : session.state === 'failed' ? 'failed' : '';
        const row = sessionRow(task.request, session, status, tone, startedAt);
        if (quiet) row.querySelector('.activity-mark').after(node('span', 'sr-only', 'Complete, started '));
        ended.push(row);
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
    else if (revealedSessions && rows.length) triggerRowReveal(list);
    list.closest('section').hidden = !loading && !rows.length && more.hidden;
  }
  const nextActivityBotId = view && !view.dm ? view.id : null;
  if (nextActivityBotId !== activityBotId) {
    const shouldReveal = activitySeen && !!nextActivityBotId;
    activityBotId = nextActivityBotId;
    if (nextActivityBotId) activitySeen = true;
    activityReveal?.cancel();
    activityReveal = null;
    const panel = elements['bot-activity'];
    if (shouldReveal && !panel.hidden && !reducedMotion.matches) {
      activityReveal = panel.animate?.([
        { opacity: 0, transform: 'translateY(6px)' },
        { opacity: 1, transform: 'none' },
      ], { duration: 190, easing: 'cubic-bezier(0.23, 1, 0.32, 1)' }) ?? null;
    }
  }
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
  person: '<circle cx="12" cy="8" r="3.25"/><path d="M5.25 19c.6-3.5 3.1-5.4 6.75-5.4s6.15 1.9 6.75 5.4"/>',
  copy: '<path d="M9 9.5A1.5 1.5 0 0 1 10.5 8h7A1.5 1.5 0 0 1 19 9.5v9a1.5 1.5 0 0 1-1.5 1.5h-7A1.5 1.5 0 0 1 9 18.5z"/><path d="M15 8V5.5A1.5 1.5 0 0 0 13.5 4h-7A1.5 1.5 0 0 0 5 5.5v9A1.5 1.5 0 0 0 6.5 16H9"/>',
  copied: '<path d="m5.5 12.5 4 4 9-9"/>',
  reply: '<path d="M9.5 7 4.5 12l5 5"/><path d="M5 12h9a5.5 5.5 0 0 1 5.5 5.5V19"/>',
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
    clearTimeout(control._copyTimer);
    delete control.dataset.copied;
    void control.offsetWidth;
    control.dataset.copied = 'true';
    control.replaceChildren(icon('copied'));
    control.setAttribute('aria-label', 'Copied');
    control.title = 'Copied';
    control._copyTimer = setTimeout(() => {
      delete control.dataset.copied;
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
  const choices = task.wait.choices;
  if (hasQuestionChoices(choices)) {
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
  const reply = questionReplyButton(() => {
    if (view?.id === context.id) beginReply(target);
  }, dialog);
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
// The latest reply's first suggestion, offered in an empty composer as the likely next message.
function nextSuggestion(context) {
  const composer = context?.composer;
  if (context?.data?.canDecide !== true || context.dm || context.source || !composer ||
      sendingMessages.has(composer) || composition || decisions.composer() || activeReply(composer) ||
      composer.textAt(0) || ['working', 'thinking'].includes(context.data.presence)) return null;
  let latest = null;
  for (const event of context.events.values()) if (event.kind !== 'wait_closed' && (!latest || event.id > latest.id)) latest = event;
  if (latest?.kind !== 'conversation_reply') return null;
  const text = latest.data.suggestions?.[0];
  if (typeof text !== 'string' || !text.trim() ||
      context.data.pausedConversations?.some((item) => item.messageId === latest.data.messageId)) return null;
  const drafts = screenshotsFor(composer);
  if ([0, latest.id].some((anchor) => {
    const draft = drafts?.get(anchor);
    return draft?.images.length || draft?.loading;
  })) return null;
  const replyDraft = composer.textAt(latest.id);
  if (replyDraft && replyDraft !== text) return null;
  return { text, target: { kind: 'message', messageId: latest.id, summary: latest.data.text } };
}
function suggestionActions(event) {
  const actions = node('div', 'event-actions');
  const target = { kind: 'message', messageId: event.id, summary: event.data.text };
  for (const suggestion of event.data.suggestions)
    actions.append(button(suggestion, 'button', () => beginReply(target, suggestion)));
  return actions;
}
function channelLabel(kind) {
  if (kind === 'slack') return 'via Slack';
  if (kind === 'imessage') return 'via iMessage';
  return 'via external channel';
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

function chatTimeBreak(event) {
  const separator = node('p', 'day chat-time-break');
  separator.setAttribute('aria-hidden', 'true');
  separator.append(eventTime(event));
  return separator;
}

function messageBubble(target) {
  if (!(target instanceof Element)) return null;
  const bubble = target.closest('.event-message .bubble');
  const control = target.closest('a, button, input, select, textarea, label, summary, [controls], '
    + '[contenteditable]:not([contenteditable="false"]), [tabindex], [role="button"], [role="link"]');
  return bubble?.contains(control) ? null : bubble;
}

function closeMessageTime() {
  const context = timelineContext();
  if (!context?.revealedTimeRow) return;
  const row = byId(context.revealedTimeRow)?.closest('.event-message');
  const time = row?.querySelector(':scope > .msg-time');
  if (time) row.querySelector('.msg-actions').prepend(time);
  context.revealedTimeRow = null;
}

let messageTap = null;
const tapMoved = (event, tap) => Math.hypot(event.clientX - tap.x, event.clientY - tap.y) > 10;
const hasTextSelection = () => window.getSelection()?.isCollapsed === false;

// Observe the native click after a short, stationary press. Leave scrolling and selection to the browser.
elements.timeline.addEventListener('pointerdown', (event) => {
  const bubble = noHover.matches && event.isPrimary && event.button === 0 && messageBubble(event.target);
  messageTap = bubble && !hasTextSelection()
    ? { bubble, id: event.pointerId, x: event.clientX, y: event.clientY, start: event.timeStamp, released: false }
    : null;
}, { passive: true });
elements.timeline.addEventListener('pointermove', (event) => {
  if (messageTap?.id === event.pointerId && tapMoved(event, messageTap)) messageTap = null;
}, { passive: true });
elements.timeline.addEventListener('pointerup', (event) => {
  if (messageTap?.id !== event.pointerId) return;
  if (tapMoved(event, messageTap) || event.timeStamp - messageTap.start >= 500) messageTap = null;
  else messageTap.released = true;
}, { passive: true });
for (const type of ['pointercancel', 'contextmenu']) {
  elements.timeline.addEventListener(type, () => { messageTap = null; }, { passive: true });
}
elements.timeline.addEventListener('click', (event) => {
  const tap = messageTap;
  messageTap = null;
  const context = timelineContext();
  if (!context || !noHover.matches || !tap?.released || !event.detail || hasTextSelection()
    || messageBubble(event.target) !== tap.bubble) return;
  const row = tap.bubble.closest('.event-message');
  const wasRevealed = !!row.querySelector(':scope > .msg-time');
  closeMessageTime();
  if (wasRevealed) return;
  const line = tap.bubble.parentElement;
  const time = line.querySelector(':scope > .msg-actions > .msg-time');
  line.after(time);
  context.revealedTimeRow = row.id;
  time.scrollIntoView({ block: 'nearest' });
});
noHover.addEventListener('change', () => {
  messageTap = null;
  if (!noHover.matches) closeMessageTime();
});

function conversationMessageRow(id) {
  // A collapsed Session report can be an anchor inside the completion row.
  return byId(`event-${id}`)?.closest('article');
}
function resetConversationJump(context) {
  if (context) context.pendingSource = null;
  elements['conversation-source-error'].textContent = '';
}
function leaveConversationSource(context) {
  resetConversationJump(context);
  if (!context.source) return false;
  context.source = null;
  readingPositions.delete(timelineKey(context.id));
  return true;
}
function backToLatest() {
  leaveConversationSource(view);
  render('bottom');
  elements.conversation.scrollTop = elements.conversation.scrollHeight;
  elements.conversation.focus({ preventScroll: true });
}
function updateFindOverlap() {
  const scroller = elements.conversation;
  const overlap = findPanel.hidden ? 0 : findPanel.getBoundingClientRect().bottom - scroller.getBoundingClientRect().top;
  const previousOverlap = parseFloat(scroller.style.getPropertyValue('--find-overlap')) || 0;
  if (previousOverlap !== overlap) {
    const top = scroller.scrollTop;
    const atBottom = scroller.scrollHeight - top - scroller.clientHeight <= 4;
    const before = parseFloat(getComputedStyle(scroller).scrollPaddingBlockStart) || 0;
    scroller.style.setProperty('--find-overlap', `${overlap}px`);
    if (atBottom && !view?.source) scroller.scrollTop = scroller.scrollHeight;
    else {
      const after = parseFloat(getComputedStyle(scroller).scrollPaddingBlockStart) || 0;
      scroller.scrollTop = top + after - before;
    }
  }
  return overlap;
}
function focusConversationMessage(target) {
  const row = target?.closest('article') ?? target;
  if (!row) return;
  // Scroll within the conversation and keep the message clear of the floating controls.
  const scroller = elements.conversation;
  updateFindOverlap();
  const viewport = scroller.getBoundingClientRect();
  const topSpace = parseFloat(getComputedStyle(scroller).scrollPaddingBlockStart) || 0;
  const returnSpace = parseFloat(getComputedStyle(scroller).scrollPaddingBlockEnd) || 0;
  const message = (row.querySelector('.bubble') ?? row).getBoundingClientRect();
  const top = viewport.top + topSpace;
  const bottom = viewport.bottom - returnSpace;
  const center = top + Math.max(0, (bottom - top - message.height) / 2);
  row.focus({ preventScroll: true });
  const nextTop = Math.max(0, scroller.scrollTop + message.top - center);
  if (typeof scroller.scrollTo === 'function') {
    scroller.scrollTo({ top: nextTop, behavior: reducedMotion.matches ? 'auto' : 'smooth' });
  } else {
    scroller.scrollTop = nextTop;
  }
}
async function showConversationMessage(context, id, { bot, signal, preferLoaded } = {}) {
  if (context !== view || context.dm || signal?.aborted) return;
  resetConversationJump(context);
  const error = elements['conversation-source-error'];
  let target = preferLoaded ? conversationMessageRow(id) : null;
  if (!target && preferLoaded && context.events.has(id)) {
    if (leaveConversationSource(context)) {
      renderTimeline();
      renderComposer();
    }
    target = conversationMessageRow(id);
  } else if (!target) {
    // Only the newest jump in this conversation can apply its page.
    const request = context.pendingSource = { id };
    let page;
    try {
      page = await api(botQuery('conversation', context.id, id + 1), undefined, { signal });
    } catch {}
    if (context !== view || context.pendingSource !== request) return;
    context.pendingSource = null;
    if (signal?.aborted) return;
    if (!page) {
      error.textContent = 'Could not load that message.';
      return;
    }
    const event = page.events.find((item) => item.id === id);
    if (!event || event.kind === 'wait_closed' || event.kind === 'bot_message') {
      error.textContent = 'That message is no longer in this conversation.';
      return;
    }
    if (!context.source) rememberReadingPosition(true);
    context.source = { id, bot: bot ?? context.data.bot, events: page.events };
    renderTimeline();
    renderComposer();
    target = conversationMessageRow(id);
  }
  if (!target) {
    error.textContent = 'That message is no longer in this conversation.';
    return;
  }
  elements.timeline.querySelector('.conversation-source-target')?.classList.remove('conversation-source-target');
  void target.offsetWidth;
  target.classList.add('conversation-source-target');
  focusConversationMessage(target);
  updateJumpToLatest();
}
function cardSourceAction(item, close) {
  const id = item.conversationEventId;
  if (!Number.isSafeInteger(id) || id <= 0 || id >= Number.MAX_SAFE_INTEGER) return null;
  return button('Show in conversation', 'text-button board-inbox-source', () => {
    close();
    void showConversationMessage(view, id, { bot: participant(item.conversationBotId ?? item.botId), preferLoaded: true });
  });
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
  const { side } = place;
  if (data.sessionWake || data.sessionInvitation) {
    const session = data.sessionWake ?? data.sessionInvitation;
    const name = `T3 Session “${session.title || session.threadId}”`;
    const wakeText = {
      finished: `Work finished in ${name}`,
      failed: `Work stopped before finishing in ${name}`,
      waiting_for_person: `Work in ${name} needs your input`,
      unknown: `Flock couldn’t check the status of work in ${name}`,
    };
    const text = data.sessionWake
      ? (Object.hasOwn(wakeText, session.reason) ? wakeText[session.reason] : wakeText.unknown)
      : `Invited by ${session.inviterName} to ${name}`;
    const note = node('article', 'system-note session-event', `${text} · `);
    note.id = `event-${event.id}`;
    note.tabIndex = -1;
    note.append(eventTime(event));
    if (session.sourceMessageId) {
      const original = button('Original request', 'text-button', () => {
        void showConversationMessage(view, session.sourceMessageId, { preferLoaded: true });
      });
      original.dataset.timelineAction = 'open-original';
      note.append(' · ', original);
    }
    if (session.note) note.append(node('p', 'session-invitation-note', session.note));
    return note;
  }
  if (side === 'system') {
    const stopped = data.request ? `“${data.request.split('\n')[0]}” was stopped` : 'A request was stopped';
    const note = node('article', 'system-note', `${bot?.name ?? 'Bot'} · ${stopped} · `);
    note.id = `event-${event.id}`;
    note.tabIndex = -1;
    note.append(eventTime(event));
    return note;
  }
  const row = node('article', `msg event-message ${side}`);
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
  const author = side === 'external' ? 'Other sender' : side === 'bot' ? bot?.name ?? 'Bot' : 'You';
  const time = eventTime(event, scheduled);
  row.append(node('span', 'msg-name sr-only', author));

  const line = node('div', 'msg-line');
  if (side === 'bot') line.append(node('span', 'msg-avatar-space'));
  else if (side === 'external') line.append(icon('person', 'msg-avatar external-sender'));
  const bubble = node('div', 'bubble');
  const text = node(['conversation_message', 'conversation_reply'].includes(event.kind) ? 'div' : 'p', 'event-text');
  const actions = node('div', 'msg-actions');
  actions.append(time);
  const extras = [];
  let replyTarget = null;
  let copy = data.text ?? '';
  const pendingWait = event.kind === 'waiting' && !context.readOnly ? context.waits.get(data.id) : null;

  if (event.kind === 'waiting') {
    copy = waitText(data, { spokenByBot: true });
    text.textContent = copy;
    bubble.append(text);
    if (data.decisionReason) bubble.append(reasonText(data.decisionReason));
    const choices = pendingWait?.wait.choices ?? data.choices;
    if (!context.canDecide && choices?.length) bubble.append(readOnlyChoices(choices));
    if (context.canDecide && pendingWait?.wait.reason === 'user_question') extras.push(questionActions(view, pendingWait));
    else if (pendingWait) {
      if (pendingWait.wait.reason !== 'policy_approval') {
        const open = node('div', 'event-actions question-actions');
        const label = pendingActionLabels[pendingWait.wait.reason] ?? 'Review…';
        open.append(button(label, 'button', () => openDetail({ kind: 'wait', taskId: pendingWait.id, waitId: data.id })));
        extras.push(open);
      }
    } else if (!context.readOnly || context.closed.has(data.id)) {
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
    if (typeof data.textHtml === 'string') {
      text.classList.add('markdown-body');
      text.innerHTML = data.textHtml;
    } else text.textContent = data.text ?? '';
    text.classList.add('foldable-text');
    bubble.append(text);
    const paused = context.paused.get(data.messageId);
    if (paused && context.canDecide) extras.push(pausedActions(paused));
    else if (data.suggestions?.length) {
      if (context.canDecide) extras.push(suggestionActions(event));
      else bubble.append(readOnlyChoices(data.suggestions));
    }
    if (!paused) replyTarget = ['Reply to this message', { kind: 'message', messageId: event.id, summary: data.text }];
  } else if (event.kind === 'pr_review_requested') {
    text.textContent = data.title;
    bubble.append(text);
    if (data.text && data.text !== data.title) bubble.append(node('p', 'event-reason', data.text));
    if (data.prUrl) {
      const link = pullRequestLink(data.prUrl, 'View pull request');
      if (link instanceof HTMLAnchorElement) {
        link.className = 'link';
        bubble.append(link);
      }
    }
    replyTarget = ['Reply to this review', { kind: 'message', messageId: event.id, summary: data.title }];
  } else if (event.kind === 'report') {
    copy = data.evidenceStatus === 'report_unavailable' ? 'The work ended, but its report couldn’t be loaded.'
      : data.report || 'The session submitted a report.';
    text.textContent = copy;
    text.classList.add('foldable-text');
    const link = runIcon(data.run);
    if (link) text.append(link);
    bubble.append(text);
    if (data.report) replyTarget = ['Reply to this result', { kind: 'result', messageId: event.id, summary: data.report }];
    if (typeof data.status === 'string' && data.status && data.status !== 'completed') {
      const status = Object.hasOwn(stateLabels, data.status) ? stateLabels[data.status] : data.status;
      bubble.append(node('p', 'event-reason', `Work ${status.toLowerCase()}`));
    }
    if (data.evidenceStatus && data.detail) bubble.append(node('p', 'event-reason', data.detail));
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
    if (event.kind === 'conversation_message' && typeof data.textHtml === 'string') {
      text.classList.add('markdown-body');
      text.innerHTML = data.textHtml;
    } else text.append(document.createTextNode(data.text ?? ''));
    bubble.append(text);
    text.classList.add('foldable-text');
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
    if (event.kind === 'conversation_message' && data.replyToMessageId) {
      const original = button('Open original message', 'text-button open-original-message', () => {
        void showConversationMessage(view, data.replyToMessageId, { preferLoaded: true });
      });
      original.dataset.timelineAction = 'open-original';
      extras.push(original);
    }
    if (data.channel?.channelKind) extras.push(node('p', 'msg-note channel-source', channelLabel(data.channel.channelKind)));
    else if (event.id === context.latestWebMessageId && data.status === 'received')
      extras.push(node('p', 'msg-note', 'Received'));
    if (data.deliveredToT3) extras.push(node('p', 'msg-note', 'Sent to T3'));
    const blocker = ['request', 'conversation_message'].includes(event.kind) ? context.queued.get(event.taskId)?.blockedBy : null;
    if (blocker) {
      const owner = bots.find((item) => item.id === blocker.botId)?.name ?? blocker.botId;
      // Truncate only the blocking request so "Queued after" and the owner stay readable on narrow screens.
      const note = node('p', 'msg-note msg-queued');
      note.append(icon('clock'), node('span', null, 'Queued after'), node('span', 'msg-queued-request', `“${blocker.request}”`));
      if (blocker.botId !== context.bot?.id) note.append(node('span', 'msg-queued-owner', `· ${owner}`));
      note.title = `Starts when “${blocker.request}” (${owner}) finishes`;
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
  if (!actions.querySelector('button:not(:disabled)')) row.tabIndex = 0;
  line.append(bubble, actions);
  row.append(line, ...extras);
  const revealed = context.revealedTimeRow;
  if (noHover.matches && (revealed === row.id || (report && revealed === `event-${report.id}`))) line.after(time);
  return row;
}
function syncTimelineAvatar(row, show, bot) {
  const current = row.querySelector('.msg-line')?.firstElementChild;
  if (show && current?.classList.contains('msg-avatar-space')) current.replaceWith(avatar(bot, 'msg-avatar'));
  else if (!show && current?.classList.contains('msg-avatar')) current.replaceWith(node('span', 'msg-avatar-space'));
}
const workingText = (data) => data.presence === 'working'
  ? data.activeSessions?.[0]?.request ?? presenceLabels.working : presenceLabels.thinking;
// The working row stays mounted while the Bot works; only its status changes, sliding the old one out.
function workingLine(bot) {
  const row = node('div', 'msg bot working group-start');
  const line = node('div', 'msg-line');
  const dots = node('span', 'typing');
  dots.setAttribute('aria-hidden', 'true');
  dots.append(node('span'), node('span'), node('span'));
  const status = node('span', 'working-status');
  line.append(avatar(bot, 'msg-avatar'), dots, status);
  row.append(line);
  return row;
}
function setWorkingText(row, label) {
  const status = row.querySelector('.working-status');
  const current = status.querySelector('.working-label:not(.leaving)');
  if (current?.textContent === label) return;
  status.querySelector('.working-label.leaving')?.remove();
  const next = node('span', 'working-label', label);
  next.title = label;
  if (current && !reducedMotion.matches) {
    current.setAttribute('aria-hidden', 'true');
    current.classList.add('leaving');
    next.classList.add('entering');
    const removeLeaving = ({ animationName }) => {
      if (animationName === 'working-label-out') current.remove();
    };
    const clearEntering = ({ animationName }) => {
      if (animationName !== 'working-label-in') return;
      next.classList.remove('entering');
      next.removeEventListener('animationend', clearEntering);
      next.removeEventListener('animationcancel', clearEntering);
    };
    current.addEventListener('animationend', removeLeaving);
    current.addEventListener('animationcancel', removeLeaving);
    next.addEventListener('animationend', clearEntering);
    next.addEventListener('animationcancel', clearEntering);
  } else current?.remove();
  status.append(next);
}
function startedLabel(timestamp) {
  const day = dayLabel(timestamp / 1000);
  return day === 'Today' ? clock.format(timestamp) : day === 'Yesterday' ? day : shortDay.format(timestamp);
}
function dayLabel(seconds) {
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const days = Math.round((today - new Date(seconds * 1000).setHours(0, 0, 0, 0)) / 86400000);
  return days === 0 ? 'Today' : days === 1 ? 'Yesterday' : dayFormat.format(seconds * 1000);
}

function peerExchangeLink(peer, activeBot, eventIds) {
  const [firstMessageId] = eventIds;
  const messageCount = eventIds.length;
  const count = messageCount === 1 ? '1 message' : `${messageCount} messages`;
  const link = peerLink(activeBot, peer.id, null, 'peer-exchange-link', firstMessageId);
  link.append(avatar(peer, 'peer-exchange-avatar'), node('span', null, `${count} with ${peer.name}`));
  link.dataset.peerFocus = firstMessageId;
  link.dataset.peerEvents = eventIds.join(' ');
  return link;
}

function peerExchangeForEvent(eventId) {
  return eventId ? elements.timeline.querySelector(`[data-peer-events~="${eventId}"]`) : null;
}

function leavePeerConversation(updateHistory = true) {
  const { id, dm } = view;
  const anchor = dm.fromPeerLink && readingPositions.has(timelineKey(id)) ? dm.anchor : 0;
  selectBot(id, null, { updateHistory });
  const picker = elements['peer-picker'];
  const target = peerExchangeForEvent(anchor) ?? (picker.hidden ? elements['profile-avatar-button'] : picker);
  target.focus({ preventScroll: true });
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
  events.forEach((event, index) => {
    const nextDay = dayLabel(event.time);
    if (nextDay !== day) {
      rows.push(keep(`day-${event.id}`, nextDay, () => node('p', 'day', nextDay)));
      day = nextDay;
    }
    const place = peerMessagePlacement(events[index - 1], event, events[index + 1]);
    if (place.timeBreak) rows.push(keep(`gap-${event.id}`, event.time, () => chatTimeBreak(event)));
    const from = participant(event.data.fromBotId);
    const anchored = event.id === dm.anchorId;
    const row = keep(`event-${event.id}`, [event, from.name, from.avatar, anchored], () => peerMessage(event, from, dm, anchored));
    row.classList.toggle('group-start', place.groupStart);
    row.classList.toggle('group-continues', !place.groupEnd);
    row.querySelector('.msg-meta').classList.toggle('sr-only', !place.groupStart);
    syncTimelineAvatar(row, place.groupEnd, from);
    rows.push(row);
  });
  return rows;
}
function peerMessage(event, from, dm, anchored) {
  const { text, forwardedMessage } = event.data;
  const row = node('article', 'msg event-message peer-dm-message');
  row.id = `event-${event.id}`;
  row.tabIndex = 0;
  row.toggleAttribute('data-anchored', anchored);
  const metadata = node('div', 'msg-meta');
  metadata.append(node('span', 'msg-name', from.name));
  const line = node('div', 'msg-line');
  const bubble = node('div', 'bubble');
  if (forwardedMessage) {
    const quote = node('blockquote', 'peer-forwarded-message');
    const label = node('p', 'msg-quote', forwardedMessage.fromViewer === true ? 'You' : 'Original sender');
    quote.append(label, node('p', 'event-text', forwardedMessage.text));
    bubble.append(quote);
  }
  if (text || !forwardedMessage) bubble.append(node('p', 'event-text', text));
  const time = eventTime(event);
  const actions = node('div', 'msg-actions');
  actions.append(time);
  line.append(node('span', 'msg-avatar-space'), bubble, actions);
  row.append(metadata, line);
  if (noHover.matches && dm.revealedTimeRow === row.id) line.after(time);
  return row;
}

function timelineScrollPosition() {
  const scroller = elements.conversation;
  const height = scroller.scrollHeight;
  const top = scroller.scrollTop;
  return { height, top, atBottom: height - top - scroller.clientHeight < 65 };
}

function timelineAtEnd(scroller, viewportHeight = scroller.clientHeight) {
  return scroller.scrollHeight - scroller.scrollTop - viewportHeight <= 4;
}

function rememberReadingPosition(keepAtBottom = false) {
  if (view?.source) return;
  const context = timelineContext();
  if (context?.newestEvent == null || context.anchor || (!context.peerId && !context.data)) return;
  const { top, atBottom } = timelineScrollPosition();
  if (atBottom && !keepAtBottom) return;
  const viewportTop = elements.conversation.getBoundingClientRect().top;
  const anchors = elements.timeline.querySelectorAll(':scope > [id], :scope > [data-peer-focus]');
  const anchor = [...anchors].find((row) => row.getBoundingClientRect().bottom > viewportTop);
  const { events, olderBefore, newestEvent } = context;
  readingPositions.set(timelineKey(context.id, context.peerId), {
    // Retain loaded pages so an older message is available when its conversation reopens.
    history: { events, olderBefore, newestEvent, historyCursor: context.data?.historyCursor },
    top, atBottom,
    anchorId: anchor?.id,
    peerFocus: anchor?.dataset.peerFocus,
    offset: anchor ? anchor.getBoundingClientRect().top - viewportTop : 0,
  });
}

function restoreReadingPosition(position) {
  const scroller = elements.conversation;
  if (position.atBottom) {
    scroller.scrollTop = scroller.scrollHeight;
    return;
  }
  const row = position.anchorId ? byId(position.anchorId) : peerExchangeForEvent(position.peerFocus);
  const anchor = row?.closest('article') ?? row;
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
  // An explicitly opened history page is separate from the latest messages.
  const hidden = !view?.source && !view?.dm?.historical && atBottom;
  if (control.hidden === hidden) return;
  if (hidden && document.activeElement === control) scroller.focus({ preventScroll: true });
  control.hidden = hidden;
}

function renderTimeline(scroll = 'auto', previousScroll = timelineScrollPosition(), liveAfterId) {
  const older = elements.older;
  const source = view?.source;
  const data = source ? null : view?.data;
  const context = timelineContext();
  const events = source?.events ?? [...(context?.events.values() ?? [])].sort((a, b) => a.id - b.id);
  elements.conversation.classList.toggle('viewing-source', !!source);
  older.hidden = !!source || !context?.olderBefore;
  older.disabled = !!context?.loadingOlder;
  older.textContent = context?.loadingOlder ? 'Loading…' : 'Show earlier messages';
  const peerStatus = elements['peer-status'];
  const olderFailed = view?.dm?.olderFailed;
  peerStatus.hidden = !olderFailed;
  peerStatus.textContent = olderFailed ? 'Could not load earlier messages.' : '';
  const scroller = elements.conversation;
  const key = context?.newestEvent != null && !context.anchor && timelineKey(context.id, context.peerId);
  const position = !source && key && readingPositions.get(key);
  const moveScroll = () => {
    if (scroll === 'anchor') focusConversationMessage(byId(`event-${context.anchorId}`));
    else if (scroll === 'older') scroller.scrollTop = previousScroll.top + scroller.scrollHeight - previousScroll.height;
    else if (position) {
      restoreReadingPosition(position);
      // The restored offset already includes the returning composer's height.
      timelineHeight = scroller.clientHeight;
      timelineWidth = scroller.clientWidth;
      scroller.classList.toggle('follow-newest', timelineAtEnd(scroller, timelineHeight));
      readingPositions.delete(key);
    } else if (scroll === 'bottom' || (!context?.anchor && previousScroll.atBottom)) scroller.scrollTop = scroller.scrollHeight;
    else scroller.scrollTop = previousScroll.top;
    updateJumpToLatest();
  };
  if (view?.dm) {
    const dm = view.dm;
    const participants = [participant(dm.id), participant(dm.peerId)].map(({ id, name, avatar }) => [id, name, avatar]);
    if (!changed('timeline', dm.id, dm.peerId, dm.anchor, events, dm.newestEvent, dm.error, participants, dayFormat.format(Date.now()))) return;
    scroller.setAttribute('aria-label', 'Bot DM history');
    updateTimelineRows(dm, (keep) => peerThread(events, dm, keep));
    moveScroll();
    if (dm.pendingViewMotion && elements.timeline.childElementCount > 0) {
      const pending = dm.pendingViewMotion;
      dm.pendingViewMotion = null;
      animateTimelineContent(pending);
    }
    return;
  }
  if (changed('timeline', view?.id, source ?? [data?.composerPerson, data?.bot, events, data?.waits, data?.queued, [...pendingWaits], listLoaded,
    data?.presence, data?.activeSessions, data?.sessionTasks, data?.pausedConversations, bots, dayFormat.format(Date.now()),
    decisions.itemsFor(view?.id).filter(item => item.itemKind === 'policy_approval')])) {
    scroller.setAttribute('aria-label', 'Conversation history');
    updateTimelineRows(context, (keep) => conversationRows(data, events, keep, source));
    foldLongMessages();
    moveScroll();
    if (context?.pendingViewMotion && elements.timeline.childElementCount > 0) {
      const pending = context.pendingViewMotion;
      context.pendingViewMotion = null;
      animateTimelineContent(pending);
    }
  }
  const motionAllowed = !source && !document.hidden && !reducedMotion.matches && !document.querySelector('dialog[open]');
  const canAnimate = motionAllowed && liveAfterId != null && scroll === 'auto' && previousScroll.atBottom;
  if (canAnimate) {
    const id = replyArrivalId(events, liveAfterId);
    if (id != null) animateLiveReply(id);
  }
  const sentAfterId = context?.pendingSentAfterId;
  const messageAfterId = canAnimate ? liveAfterId : (sentAfterId != null ? sentAfterId : null);
  if (messageAfterId != null) {
    const addedMessages = events.filter((event) => event.id > messageAfterId &&
      event.kind === 'conversation_message' && messageSide(event) !== 'system');
    if (addedMessages.length > 0) {
      if (motionAllowed && addedMessages.length <= 2) {
        for (const event of addedMessages) animateSentMessage(event.id);
      }
      if (sentAfterId != null) context.pendingSentAfterId = null;
    }
  }
}

// Like ChatGPT, fold any very long message to its first lines behind Show more.
const foldLines = 12;
const unfoldedMessages = new Set();
function foldLongMessages() {
  for (const text of elements.timeline.querySelectorAll('.foldable-text:not([data-fold-checked])')) {
    text.dataset.foldChecked = '';
    const lineHeight = parseFloat(getComputedStyle(text).lineHeight);
    if (!(text.scrollHeight > lineHeight * (foldLines + 2))) continue;
    const bubble = text.closest('.bubble');
    const id = text.closest('.event-message')?.id;
    const setFold = (folded) => {
      bubble.dataset.fold = folded ? 'folded' : 'unfolded';
      toggle.textContent = folded ? 'Show more' : 'Show less';
      toggle.setAttribute('aria-expanded', String(!folded));
    };
    const toggle = button(null, 'fold-toggle', () => {
      const nextFolded = bubble.dataset.fold !== 'folded';
      const startHeight = text.getBoundingClientRect().height;
      cancelMotion(text);
      setFold(nextFolded);
      if (nextFolded) unfoldedMessages.delete(id);
      else unfoldedMessages.add(id);
      const endHeight = nextFolded ? text.getBoundingClientRect().height : text.scrollHeight;
      if (!reducedMotion.matches && text.animate && Math.abs(endHeight - startHeight) > 1) {
        text.style.overflow = 'hidden';
        const duration = Math.min(340, Math.max(200, Math.round(180 + Math.abs(endHeight - startHeight) * 0.18)));
        const anim = runMotion(text, [
          { maxHeight: `${startHeight}px` },
          { maxHeight: `${endHeight}px` },
        ], { duration, easing: motionEaseOut });
        if (anim) {
          const cleanup = () => {
            if (!motionAnimations.has(text)) text.style.overflow = '';
          };
          anim.addEventListener('finish', () => {
            cleanup();
            if (nextFolded) bubble.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
          }, { once: true });
          anim.addEventListener('cancel', cleanup, { once: true });
          return;
        }
        text.style.overflow = '';
      }
      if (nextFolded) bubble.scrollIntoView({ block: 'nearest' });
    });
    text.after(toggle);
    setFold(!unfoldedMessages.has(id));
  }
}

function conversationRows(data, events, keep, source) {
  const timeline = {
    bot: source?.bot ?? data?.bot ?? bots.find((item) => item.id === view?.id),
    // Source lookups do not establish the latest message in the live conversation.
    latestWebMessageId: data && !source ? events.findLast((event) =>
      event.kind === 'conversation_message' && !event.data.channel?.channelKind && messageSide(event) === 'user')?.id : undefined,
    // An earlier page opened from the inbox is for reading; answering happens at the latest messages.
    canDecide: !source && data?.canDecide === true,
    readOnly: !!source,
    revealedTimeRow: view?.revealedTimeRow,
    paused: new Map((data?.pausedConversations ?? []).map(item => [item.messageId, item])),
    waits: new Map((data?.waits ?? []).map((task) => [task.wait.id, task])),
    queued: new Map((data?.queued ?? []).map((task) => [task.id, task])),
    closed: new Map(events.filter((event) => event.kind === 'wait_closed').map((event) => [event.data.waitId, event.data.resolution])),
  };
  const bot = timeline.bot;
  const botVersion = JSON.stringify([bot?.id, bot?.name, bot?.avatar]);
  const approvals = source ? [] : decisions.itemsFor(view?.id).filter(item => item.itemKind === 'policy_approval' &&
    (item.status !== 'pending' || !timeline.closed.has(item.waitId)));
  const pendingByWait = new Map(approvals.filter(item => item.status === 'pending').map(item => [item.waitId, item]));
  const replacements = new Map();
  const anchoredToWait = new Set();
  for (const event of events) {
    if (event.kind !== 'waiting' || event.data.reason !== 'policy_approval') continue;
    const item = pendingByWait.get(event.data.id);
    if (item && item.taskId === event.taskId) {
      replacements.set(event.id, item);
      anchoredToWait.add(item.itemId);
    }
  }
  const shown = timelineMessages(events, dayLabel);
  const rows = [];
  let peerMessages = [];
  const appendPeerMessages = () => {
    if (!peerMessages.length) return;
    const [first] = peerMessages;
    const { fromBotId, toBotId } = first.data;
    const peer = participant(fromBotId === view.id ? toBotId : fromBotId);
    const eventIds = peerMessages.map(({ id }) => id);
    rows.push(keep(`peer-${first.id}`, [peer.id, peer.name, peer.avatar, eventIds],
      () => peerExchangeLink(peer, view.id, eventIds)));
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
    const place = messagePlacement(previous, event, shown[index + 1]?.event);
    if (place.timeBreak) rows.push(keep(`gap-${event.id}`, event.time, () => chatTimeBreak(event)));
    const approval = replacements.get(event.id);
    if (approval) {
      rows.push(keep(`event-${event.id}`, ['approval', approval], () => {
        const card = decisions.inlinePolicyCard(approval);
        card.id = `event-${event.id}`;
        card.tabIndex = -1;
        return card;
      }));
      previous = event;
      return;
    }
    const wait = event.kind === 'waiting' ? timeline.waits.get(event.data.id) : null;
    const paused = event.kind === 'conversation_reply' ? timeline.paused.get(event.data.messageId) : null;
    const blocker = ['request', 'conversation_message'].includes(event.kind) ? timeline.queued.get(event.taskId)?.blockedBy : null;
    const blockedOwner = blocker && bots.find((item) => item.id === blocker.botId)?.name;
    const row = keep(`event-${report?.id ?? event.id}`,
      [event, report, botVersion, timeline.canDecide, timeline.readOnly, event.id === timeline.latestWebMessageId,
        place.groupStart, place.taskStart, place.afterQuestion, wait,
        event.kind === 'waiting' ? timeline.closed.get(event.data.id) : null,
        wait && pendingWaits.has(wait.wait.id), paused, paused && pendingWaits.has(`message/${paused.messageId}`), blocker, blockedOwner,
        event.kind === 'waiting' && !!view.composer?.textAt(event.id)],
      () => eventCard(event, timeline, place, report));
    if (place.side === 'bot') syncTimelineAvatar(row, place.groupEnd && bot, bot);
    row.classList.toggle('group-continues', !place.groupEnd);
    row.classList.toggle('conversation-source-target', event.id === source?.id);
    rows.push(row);
    for (const item of approvals) {
      if (!anchoredToWait.has(item.itemId) && item.conversationEventId === event.id)
        rows.push(keep(`policy-${item.itemId}`, item, () => decisions.inlinePolicyCard(item)));
    }
    previous = event;
  });
  appendPeerMessages();
  // A new chat stays blank: the header already shows the Bot's avatar and name.
  if (!rows.length && !data) {
    const loaded = !view && listLoaded;
    rows.push(keep('empty', [loaded, !bots.length], () => {
      const empty = node('div', 'empty-state');
      if (loaded && !bots.length) {
        empty.classList.add('first-run');
        const flock = node('div', 'first-run-flock');
        flock.setAttribute('aria-hidden', 'true');
        for (const example of ['chat', 'sprout', 'controller']) flock.append(avatar({ id: example, avatar: { example } }));
        empty.append(flock, node('h2', null, 'Create your first Bot'),
          node('p', null, 'A Bot takes your requests, runs the work in T3 Sessions and reports back here.'),
          button('New Bot', 'dialog-button primary', () => openNewBot(elements['new-bot-button'])));
      } else {
        empty.append(node('h2', null, loaded ? 'No Bots' : 'Loading…'));
      }
      return empty;
    }));
  }
  if (data && (data.presence === 'working' || data.presence === 'thinking')) {
    const row = keep('working', botVersion, () => workingLine(bot));
    setWorkingText(row, workingText(data));
    rows.push(row);
  }
  return rows;
}

function setNotice(context, text, isError = false) {
  notices.set(context.id, { text, isError });
  if (showingComposer(context.id)) renderComposer();
}
const replyLabels = { wait: 'Reply to a question', message: 'Reply to a message', result: 'Reply to a result' };
const composerLimit = (reply) => reply?.kind === 'wait' ? 8000 : 16000;
const activeReply = (composer) => decisions.reviewReply() ?? composer.reply;
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
    !decisions.composer() && screenshotAnchor(activeReply(composer)) === anchor && !closedQuestion(context, reply) && !sendingMessages.has(composer);
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
  const scroller = elements.conversation;
  const atBottom = timelineAtEnd(scroller);
  field.style.height = 'auto';
  field.style.height = `${field.scrollHeight}px`;
  // Measuring the textarea briefly frees space and can clamp the timeline's scroll position.
  if (atBottom) scroller.scrollTop = scroller.scrollHeight;
}
function renderComposer() {
  const context = view;
  const readOnly = context?.data?.canDecide === false;
  const available = !!context?.data && !context.dm && (!!context.composer || readOnly);
  const canDecide = available && context.data.canDecide === true;
  elements['composer-container'].hidden = !available;
  elements.composer.hidden = !canDecide;
  elements['view-only-notice'].hidden = !available || canDecide;
  const decision = canDecide ? decisions.composer() : null;
  const review = canDecide ? decisions.reviewReply() : null;
  elements['decision-reply-context'].hidden = !decision && !review;
  if (!canDecide || !context.composer) {
    composition = null;
    elements['composer-ghost'].hidden = true;
    elements.message.classList.remove('has-ghost');
    elements.message.value = '';
    return renderScreenshots(null, true);
  }
  const { composer } = context;
  const reply = decision ? null : review ?? composer.reply;
  const anchor = decision?.key ?? reply?.messageId;
  if (composition && (composition.composer !== composer || composition.anchor !== anchor)) composition = null;
  const screenshots = decision ? null : screenshotsFor(composer)?.get(screenshotAnchor(reply));
  const notice = notices.get(context.id);
  const body = decision?.text ?? composer.textAt(screenshotAnchor(reply));
  const busy = !!decision?.busy || sendingMessages.has(composer);
  const closedReply = closedQuestion(context, reply);
  const source = reply && context.events.get(reply.messageId)?.data;
  const summary = reply?.summary ?? (source && (reply.kind === 'wait' ? waitText(source) : source.report ?? source.text));
  if (decision || review) {
    elements['decision-reply-label'].textContent = 'Replying to';
    elements['decision-reply-target'].textContent = decision?.item.requestTitle ?? review.summary;
  }
  elements['reply-context'].hidden = !reply || !!review;
  elements['reply-label'].textContent = reply ? replyLabels[reply.kind] : '';
  elements['reply-question'].textContent = reply ? summary ?? 'Earlier message' : '';
  elements['compose-label'].textContent = decision ? `Replying to ${decision.item.requestTitle}` :
    review ? `Replying to ${review.summary}` : reply ? replyLabels[reply.kind] : 'Message';
  elements['cancel-reply'].disabled = busy;
  const working = ['working', 'thinking'].includes(context.data.presence);
  elements.message.placeholder = (decision || review) ? 'Ask a question or give direction' :
    reply ? 'Write a reply' : working ? `Guide ${context.data.bot.name} while it works` : `Message ${context.data.bot.name}`;
  const suggestion = nextSuggestion(context);
  const ghost = elements['composer-ghost'];
  ghost.hidden = !suggestion;
  ghost.firstElementChild.textContent = suggestion?.text ?? '';
  elements.message.classList.toggle('has-ghost', !!suggestion);
  if (suggestion) elements.message.placeholder = `${suggestion.text} (press Enter to use this suggestion)`;
  if (!composition && elements.message.value !== body) {
    elements.message.value = body;
    fitMessage();
  }
  elements.message.readOnly = busy || closedReply;
  elements['attach-screenshots'].hidden = !!decision;
  elements['attach-screenshots'].disabled = !!decision || busy || closedReply;
  elements['attach-screenshots'].setAttribute('aria-disabled', String(!!screenshots?.loading));
  renderScreenshots(screenshots, busy || !!screenshots?.loading);
  elements.composer.setAttribute('aria-busy', String(busy));
  elements.send.disabled = busy || !!composition || closedReply || !!decision?.blocked || !!screenshots?.loading ||
    !body.trim() || byteLength(body) > (decision ? 8000 : composerLimit(reply));
  elements.composer.classList.toggle('can-send', !elements.send.disabled);
  const sendLabel = decision ? 'Send written answer' : reply ? 'Send reply' : 'Send message';
  elements.send.setAttribute('aria-label', sendLabel);
  elements.send.title = sendLabel;
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
  recurring: ['Recurring requests', '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="8"/><path d="M12 7.5V12l3 2"/></svg>'],
};
const settingsTabs = Object.keys(settingsSections);
const chevron = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9.5 6.5 5.5 5.5-5.5 5.5"/></svg>';
let detailAvatar = null;
let detailOrigin = null;
let recurringRequests = null;

function releaseDetailAvatar() {
  detailAvatar?.handle?.destroy();
  detailAvatar = null;
}
const roleSize = new ResizeObserver(([{ target: soul }]) => {
  const lineHeight = parseFloat(getComputedStyle(soul).lineHeight);
  soul.nextElementSibling.hidden = soul.scrollHeight <= Math.ceil(8 * lineHeight);
});
let detailSectionTimer = 0;
function clearDetailSectionTransition() {
  clearTimeout(detailSectionTimer);
  detailSectionTimer = 0;
  elements['detail-body'].classList.remove('section-switching');
}
function setDetail(selection) {
  recurringRequests?.close();
  recurringRequests = selection?.kind === 'settings' ? createRecurringRequests(view.id, api) : null;
  detail = selection;
}
function closeDetail() {
  setDetail(null);
  clearDetailSectionTransition();
  releaseDetailAvatar();
  if (elements.detail.open) elements.detail.close();
}
function openDetail(selection) {
  if (!view?.data) return;
  decisions.close({ focus: false });
  clearDetailSectionTransition();
  setDetail(selection.kind === 'settings' ? { section: 'general', ...selection } : selection);
  signatures.delete('detail');
  renderDetail();
  if (elements.detail.open) return;
  detailOrigin = document.activeElement;
  elements.detail.showModal();
  if (!detailNav.hidden) detailNav.querySelector('[aria-selected="true"]')?.focus();
}
elements.detail.addEventListener('cancel', (event) => {
  const keep = elements.detail.querySelector('.wait-stop-keep');
  if (!keep || keep.disabled) return;
  event.preventDefault();
  keep.click();
});
elements.detail.addEventListener('close', () => {
  if (elements.detail.open) return;
  setDetail(null);
  clearDetailSectionTransition();
  releaseDetailAvatar();
  roleSize.disconnect();
  const origin = detailOrigin;
  detailOrigin = null;
  const active = document.activeElement;
  if (origin?.isConnected && (active === document.body || elements.detail.contains(active))) origin.focus();
});

function selectSection(section, focus = false) {
  if (detail?.kind !== 'settings' || detail.section === section) return;
  detail.section = section;
  renderDetail();
  const body = elements['detail-body'];
  body.scrollTop = 0;
  clearDetailSectionTransition();
  if (!reducedMotion.matches) {
    void body.offsetWidth;
    body.classList.add('section-switching');
    detailSectionTimer = setTimeout(() => {
      body.classList.remove('section-switching');
      detailSectionTimer = 0;
    }, 220);
  }
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

// The header avatar is hidden in a Bot DM; the conversation menu stays visible there.
function headerFocusTarget() {
  return elements['profile-identity'].hidden ? conversationActionsTrigger : elements['profile-avatar-button'];
}

function openEditAppearance(bot) {
  reopenSettingsOnClose = true;
  closeDetail();
  botEditor.openEdit(bot, { bots, origin: headerFocusTarget() });
}

const pencilIcon = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 20h9M16.5 3.5a2.12 2.12 0 1 1 3 3L7 19l-4 1 1-4Z"/></svg>';
function generalContent(bot) {
  const hero = node('div', 'settings-hero');
  const avatarButton = button(null, 'settings-avatar-button', () => openEditAppearance(bot));
  avatarButton.setAttribute('aria-label', 'Change avatar');
  const face = node('span', 'settings-avatar');
  const avatarTip = node('span', 'settings-avatar-tooltip', 'Change avatar');
  avatarTip.setAttribute('aria-hidden', 'true');
  avatarButton.append(face, avatarTip);
  detailAvatar = { mount: () => showAvatar(face, bot, { still: false }) };

  const nameWrap = node('div', 'settings-name-wrap');
  const statusNote = node('p', 'settings-status', detail?.status ?? '');
  statusNote.setAttribute('role', 'status');
  statusNote.hidden = !detail?.status;

  function renderNameDisplay() {
    const nameBtn = button(null, 'settings-name-button', beginInlineRename);
    nameBtn.title = 'Rename Bot';
    nameBtn.append(node('span', 'settings-name', bot.name));
    nameBtn.insertAdjacentHTML('beforeend', pencilIcon);
    nameWrap.replaceChildren(nameBtn);
  }
  function beginInlineRename() {
    const input = node('input', 'settings-name-input');
    input.type = 'text';
    input.maxLength = 32;
    input.value = bot.name;
    input.setAttribute('aria-label', 'Bot name');
    let settled = false;
    async function commit() {
      if (settled) return;
      settled = true;
      const trimmed = input.value.trim();
      if (!trimmed || trimmed === bot.name) {
        renderNameDisplay();
        return;
      }
      input.disabled = true;
      statusNote.classList.remove('error');
      statusNote.textContent = 'Saving name…';
      statusNote.hidden = false;
      try {
        await api('bots/profile', { botId: bot.id, name: trimmed });
        await Promise.all([refreshBots(), view?.id === bot.id && view.refresh()]);
        if (detail?.kind === 'settings') {
          detail = { ...detail, status: 'Name updated' };
          signatures.delete('detail');
          renderDetail();
        }
      } catch (error) {
        statusNote.classList.add('error');
        statusNote.textContent = error.message === 'invalid_name'
          ? 'Enter a name between 1 and 32 characters.'
          : 'Could not rename this Bot.';
        statusNote.hidden = false;
        renderNameDisplay();
      }
    }
    input.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') {
        event.preventDefault();
        void commit();
      } else if (event.key === 'Escape') {
        event.preventDefault();
        event.stopPropagation();
        settled = true;
        renderNameDisplay();
        nameWrap.querySelector('button')?.focus();
      }
    });
    input.addEventListener('blur', () => { void commit(); });
    nameWrap.replaceChildren(input);
    input.focus();
    input.select();
  }
  renderNameDisplay();
  hero.append(avatarButton, nameWrap, statusNote);
  const rows = node('div', 'settings-card settings-rows');
  if (!bot.soul) rows.append(settingsRow('Role', 'None'));
  const requests = settingsRow('Recurring requests', '', () => selectSection('recurring', true));
  requests.querySelector('.settings-row-value').append(recurringRequests.count);
  rows.append(requests);
  const settings = document.createElement('a');
  settings.className = 'settings-row';
  settings.href = '/settings/' + encodeURIComponent(bot.id);
  settings.append(node('span', 'settings-row-label', 'Sharing and connections'));
  settings.insertAdjacentHTML('beforeend', chevron);
  rows.append(settings);
  if (!bot.soul) return [hero, rows];
  const role = node('section', 'settings-card settings-role');
  const heading = node('h3', 'settings-card-label', 'Role');
  heading.id = 'settings-role-label';
  role.setAttribute('aria-labelledby', heading.id);
  const soul = node('p', 'settings-soul', bot.soul);
  soul.id = 'settings-role-text';
  soul.dataset.folded = 'true';
  const toggle = button('Show all', 'text-button settings-soul-toggle', () => {
    const folded = soul.dataset.folded === 'true';
    soul.dataset.folded = String(!folded);
    toggle.textContent = folded ? 'Show less' : 'Show all';
    toggle.setAttribute('aria-expanded', String(folded));
  });
  toggle.hidden = true;
  toggle.setAttribute('aria-expanded', 'false');
  toggle.setAttribute('aria-controls', soul.id);
  role.append(heading, soul, toggle);
  return [hero, role, rows];
}
function settingsContent(bot, section = 'general') {
  elements['detail-title'].textContent = settingsSections[section][0];
  if (section === 'recurring') return [recurringRequests.list, recurringRequests.form];
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
  else if (wait.reason === 'model_failure') primary.append(action('Try again', 'retry'));
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
    primary.append(action('Check again', 'reconcile'));
  }
  // Stopping can't be undone, so it asks in place, with the safe choice focused, instead of a browser dialog.
  const stop = button('Stop this request', 'wait-cancel', () => {
    const botName = context.data.bot?.name ?? 'The Bot';
    const confirm = node('div', 'wait-stop-confirm');
    confirm.setAttribute('role', 'group');
    const prompt = node('p', 'wait-stop-text');
    prompt.id = 'wait-stop-text';
    prompt.append(node('strong', null, 'Stop this request?'),
      ` This requests a stop for ${botName}’s work on this request, including any T3 work assigned to it.`);
    confirm.setAttribute('aria-labelledby', prompt.id);
    const keep = button('Keep working', 'dialog-button quiet wait-stop-keep', () => {
      actions.replaceChildren(stop, primary);
      stop.focus();
    });
    const confirmStop = button('Stop request', 'dialog-button danger', () => perform('cancel'));
    const choices = node('div', 'wait-primary');
    choices.append(keep, confirmStop);
    confirm.append(prompt, choices);
    actions.replaceChildren(confirm);
    keep.focus();
  });
  actions.append(stop, primary);
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
  roleSize.disconnect();
  const body = elements['detail-body'];
  body.replaceChildren(...render());
  const soul = body.querySelector('.settings-soul');
  if (soul) roleSize.observe(soul);
  if (detailAvatar?.mount) detailAvatar.handle = detailAvatar.mount();
}
function renderDetail() {
  if (!detail || !view) return;
  if (detail.kind === 'settings') {
    const bot = view.data.bot;
    labelDetail(`${bot.name} settings`);
    renderDetailNav(settingsTabs, detail.section);
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
  const task = view.data.waits.find((item) => item.id === detail.taskId && item.wait.id === detail.waitId);
  if (!task) {
    if (changed('detail', detail, null)) {
      elements['detail-title'].textContent = 'Request closed';
      replaceDetail(() => [node('p', 'wait-why', 'Check the conversation for the latest update.')]);
    }
    return;
  }
  if (changed('detail', detail, task, waitLabel(task.wait.reason), waitText(task.wait)))
    replaceDetail(() => waitContent(view, task));
  const busy = pendingWaits.has(task.wait.id);
  for (const control of elements['detail-body'].querySelectorAll('button, input')) {
    if (!control.closest('.question-choice-form')) control.disabled = busy;
  }
  const choiceForm = elements['detail-body'].querySelector('.question-choice-form');
  if (choiceForm) updateQuestionChoiceForm(choiceForm, view.questionChoices?.get(questionKey(task)), busy);
}
function render(scroll, liveAfterId) {
  decisions.setContext(view?.dm ? null : view?.id, sidebarViewer);
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
async function conversationPage(timeline, before, anchor) {
  if (!timeline.peerId) return api(botQuery('conversation', timeline.id, before));
  const { messages, hasOlder, anchorId } = await api(peerQuery(timeline, before, anchor));
  return { events: messages, olderBefore: hasOlder ? messages[0].id : 0, anchorId };
}
async function refreshPeerConversation(context, scroll) {
  if (context !== view) return;
  const dm = context.dm;
  const isCurrent = () => context === view;
  const readPage = before => conversationPage(dm, before);
  const openingAnchor = dm.anchor && dm.newestEvent == null;
  try {
    let latest = await conversationPage(dm, undefined, dm.historical ? dm.anchor : 0);
    if (!isCurrent()) return;
    const historical = dm.historical || (openingAnchor && !latest.events.some((event) => event.id === dm.anchor));
    if (openingAnchor && historical) latest = await conversationPage(dm, undefined, dm.anchor);
    if (!isCurrent()) return;
    const eventHistory = dm.anchor ? dm : eventHistoryForRefresh(dm);
    await refreshEventHistory(eventHistory, latest, readPage, isCurrent);
    if (!isCurrent()) return;
    if (eventHistory !== dm) Object.assign(dm, eventHistory);
    dm.error = null;
    dm.historical = historical;
    if (openingAnchor) {
      dm.anchorId = historical ? latest.anchorId : dm.anchor;
      scroll = 'anchor';
    }
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
    await decisions.refresh();
  } catch (error) {
    if (error.status === 401 || error.status === 403) {
      sidebarViewer = null;
      decisions.setContext(null, null);
      render();
    }
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
  const isOpen = () => !payload.itemKind && view === context && detail?.waitId === payload.waitId;
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
  if (decisions.composer()) return decisions.submitAnswer();
  if (leaveConversationSource(context)) {
    render('bottom');
  }
  const { composer } = context;
  const review = decisions.reviewReply();
  const reply = review ?? composer.reply;
  const fields = { botId: context.id, text: composer.textAt(screenshotAnchor(reply)) };
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
    } else payload = composer.prepareSend(reply);
    context.pendingSentAfterId = context.newestEvent ?? 0;
    await api('messages', payload);
    if (attachments.length) {
      composer.settleScreenshotSend(payload);
      clearScreenshots(composer, reply);
    } else composer.settleSend(payload, true);
    if (review && view === context && decisions.reviewReply()?.messageId === review.messageId) {
      decisions.close({ focus: false });
    }
    setNotice(context, 'Message received.');
    if (showingComposer(context.id)) elements.conversation.scrollTop = elements.conversation.scrollHeight;
    if (!liveUpdates.live) await Promise.all([refreshBots(), showingComposer(context.id) && view.refresh('bottom')]);
  } catch (error) {
    context.pendingSentAfterId = null;
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

const findPanel = byId('conversation-find');
const conversationFind = mountConversationFind({
  toggle: byId('find-conversation'),
  panel: findPanel,
  readPage: (context, before, signal) => api(botQuery('conversation', context.id, before), undefined, { signal }),
  describeAuthor: (context, event) => {
    const side = messageSide(event);
    return side === 'user' ? 'You' : side === 'external' ? 'Other sender' : side === 'system' ? 'System' : context.data.bot.name;
  },
  openMatch(context, id, signal) {
    return showConversationMessage(context, id, { signal, preferLoaded: true });
  },
  beforeOpen() {
    closeConversationActions();
    decisions.close({ focus: false });
  },
});
new ResizeObserver(updateFindOverlap).observe(findPanel);

const decisions = mountBoardInbox({
  api,
  sourceAction: cardSourceAction,
  onRefreshError: retrySync,
  canCompose: () => !!view?.composer && !view.dm && responseViewers.get(view.data) === sidebarViewer,
  canDismiss: () => !menu && !conversationActions.open && !workspace.classList.contains('nav-open'),
  focusComposer() {
    const target = elements.composer.hidden || elements['composer-container'].hidden ? headerFocusTarget() : elements.message;
    target.focus({ preventScroll: true });
  },
  onChange() {
    if (decisions.isOpen()) conversationFind.close(false);
    renderBots();
    renderHeader();
    renderActivity();
    renderComposer();
    renderTimeline();
    scheduleReadCheck();
  },
  onAccepted(item, { decision, error }) {
    if (!view || (view.id !== item.botId && view.id !== item.conversationBotId)) return;
    const name = bots.find((bot) => bot.id === item.botId)?.name ?? 'The Bot';
    setNotice(view, error ?? decisionConfirmations[decision]?.(name) ?? 'Response sent.', !!error);
    void view.refresh();
  },
});

function reflectTheme() {
  renderTheme(document.documentElement.dataset.theme ?? 'system');
}
reflectTheme();
document.addEventListener('flock-theme', reflectTheme);
if (typeof ResizeObserver !== 'undefined') {
  new ResizeObserver(() => syncThemeIndicator(false)).observe(elements['theme-switch']);
}
elements['theme-switch'].addEventListener('click', (event) => {
  const option = event.target.closest('button[data-theme-mode]');
  if (option) setTheme(option.dataset.themeMode);
});
elements['new-bot-button'].addEventListener('click', () => openNewBot(elements['new-bot-button']));
elements['open-navigation'].addEventListener('click', () => openNavigation(true));
elements['close-navigation'].addEventListener('click', () => openNavigation(false));
navBackdrop?.addEventListener('click', () => openNavigation(false));
narrow.addEventListener('change', () => openNavigation(false, false));
const sidebarToggle = byId('toggle-chat-sidebar');
const sidebarResizer = elements['sidebar-resizer'];
const sidebarMinWidth = 216;
const sidebarMaxWidth = 416;
const sidebarCollapseThreshold = 160;
function setSidebarWidth(px, persist = false) {
  const clamped = Math.round(Math.max(sidebarMinWidth, Math.min(sidebarMaxWidth, px)));
  workspace.style.setProperty('--sidebar-width', `${clamped}px`);
  if (persist) save('flock-sidebar-width', clamped);
}
function setSidebarCollapsed(collapsed) {
  workspace.classList.toggle('sidebar-collapsed', collapsed);
  const label = collapsed ? 'Show Bot sidebar' : 'Hide Bot sidebar';
  sidebarToggle.setAttribute('aria-expanded', String(!collapsed));
  sidebarToggle.setAttribute('aria-label', label);
  sidebarToggle.title = label;
}
const storedSidebarWidth = saved('flock-sidebar-width', null);
if (Number.isFinite(storedSidebarWidth) && storedSidebarWidth >= sidebarMinWidth && storedSidebarWidth <= sidebarMaxWidth) {
  setSidebarWidth(storedSidebarWidth, false);
}
sidebarToggle.addEventListener('click', () => {
  setSidebarCollapsed(!workspace.classList.contains('sidebar-collapsed'));
});
if (sidebarResizer) {
  let dragPointer = null;
  let lastWidth = storedSidebarWidth ?? 296;
  sidebarResizer.addEventListener('pointerdown', (event) => {
    if (event.button !== 0 || narrow.matches) return;
    event.preventDefault();
    dragPointer = event.pointerId;
    sidebarResizer.setPointerCapture(dragPointer);
    workspace.classList.add('sidebar-resizing');
  });
  sidebarResizer.addEventListener('pointermove', (event) => {
    if (event.pointerId !== dragPointer) return;
    const width = event.clientX - workspace.getBoundingClientRect().left;
    if (width < sidebarCollapseThreshold) {
      workspace.classList.remove('sidebar-resizing');
      setSidebarCollapsed(true);
      dragPointer = null;
      return;
    }
    if (workspace.classList.contains('sidebar-collapsed')) setSidebarCollapsed(false);
    lastWidth = Math.max(sidebarMinWidth, Math.min(sidebarMaxWidth, width));
    setSidebarWidth(lastWidth, false);
  });
  const endResize = (event) => {
    if (event.pointerId !== dragPointer) return;
    dragPointer = null;
    workspace.classList.remove('sidebar-resizing');
    if (!workspace.classList.contains('sidebar-collapsed')) setSidebarWidth(lastWidth, true);
  };
  sidebarResizer.addEventListener('pointerup', endResize);
  sidebarResizer.addEventListener('pointercancel', endResize);
  sidebarResizer.addEventListener('dblclick', () => {
    workspace.style.removeProperty('--sidebar-width');
    save('flock-sidebar-width', null);
    setSidebarCollapsed(false);
  });
}
function openCurrentBotSettings() {
  closeConversationActions();
  openDetail({ kind: 'settings' });
}
elements['profile-avatar-button'].addEventListener('click', openCurrentBotSettings);
elements['peer-picker'].addEventListener('click', () => {
  closeConversationActions(true);
  openDetail({ kind: 'peers' });
});
byId('conversation-recent-decisions').addEventListener('click', () => {
  closeConversationActions();
  void decisions.openHistory(conversationActionsTrigger);
});
elements['peer-back'].addEventListener('click', () => leavePeerConversation());
elements['peer-parent-link']?.addEventListener('click', () => leavePeerConversation());
elements.conversation.addEventListener('click', (event) => {
  if (!view?.dm || window.getSelection?.()?.toString()) return;
  if (!event.target.closest('.bubble, button, a, input, textarea, summary, .msg-actions, .msg-meta, .peer-exchange-link, .older, .empty-state')) {
    leavePeerConversation();
  }
});
elements['close-detail'].addEventListener('click', closeDetail);
elements.detail.addEventListener('click', (event) => {
  if (event.target !== elements.detail) return;
  const rect = elements.detail.getBoundingClientRect();
  if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) {
    closeDetail();
  }
});
elements.detail.addEventListener('close', () => {
  if (elements.detail.open || !settingsReturnFocus) return;
  if (narrow.matches) openNavigation(true, false);
  focusKey(`menu:${settingsReturnFocus}`);
  settingsReturnFocus = null;
});
elements.older.addEventListener('click', loadOlder);
// Like Messages, reaching the top of the history loads the previous page; the button stays for keyboard and assistive use.
new IntersectionObserver((entries) => {
  if (entries.some((entry) => entry.isIntersecting)) void loadOlder();
}, { root: elements.conversation, rootMargin: '200px 0px 0px' }).observe(elements.older);
elements['jump-to-latest'].addEventListener('pointerdown', (event) => {
  if (event.button === 0) event.preventDefault();
});
elements['jump-to-latest'].addEventListener('click', () => {
  if (view?.source) {
    backToLatest();
    return;
  }
  resetConversationJump(view);
  const scroller = elements.conversation;
  const dm = view?.dm;
  if (dm?.historical) {
    readingPositions.delete(timelineKey(dm.id, dm.peerId));
    selectBot(dm.id, dm.peerId);
    scroller.focus({ preventScroll: true });
    return;
  }
  if (typeof scroller.scrollTo === 'function') {
    scroller.scrollTo({ top: scroller.scrollHeight, behavior: reducedMotion.matches ? 'auto' : 'smooth' });
  } else {
    scroller.scrollTop = scroller.scrollHeight;
  }
  updateJumpToLatest();
});
elements.conversation.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && view?.source && !event.defaultPrevented && !event.isComposing && event.keyCode !== 229) {
    event.preventDefault();
    event.stopPropagation();
    backToLatest();
  }
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
  pickerTarget = { context: view, composer: view.composer, reply: activeReply(view.composer) };
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
  if (!files.length || elements.message.readOnly || decisions.composer()) return;
  event.preventDefault();
  void addScreenshots(view, view.composer, activeReply(view.composer), files);
});
function updateDraft() {
  if (!view?.composer || view.dm || elements.message.readOnly) return;
  const decision = decisions.composer();
  const reply = activeReply(view.composer);
  if (decision) decisions.edit(elements.message.value);
  else view.composer.edit(elements.message.value, reply);
  fitMessage();
  notices.delete(view.id);
  const limit = decision ? 8000 : composerLimit(reply);
  if (byteLength(elements.message.value) > limit) setNotice(view, `Messages can be up to ${limit.toLocaleString('en')} bytes.`, true);
  else renderComposer();
}
elements.message.addEventListener('compositionstart', () => {
  if (!view?.composer || view.dm || elements.message.readOnly) return;
  composition = { composer: view.composer, anchor: decisions.composer()?.key ?? activeReply(view.composer)?.messageId };
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
  if (event.target === elements.message && !elements.message.readOnly && !elements.message.value &&
      !elements['composer-ghost'].hidden && isComposerSuggestionKey(event)) {
    const suggestion = nextSuggestion(view);
    if (suggestion) {
      event.preventDefault();
      beginReply(suggestion.target, suggestion.text);
      return;
    }
  }
  if (event.target === elements.message) handleComposerEnter(event, elements.send);
  if (event.key === 'Escape' && !elements['reply-context'].hidden) {
    event.preventDefault();
    event.stopPropagation();
    elements['cancel-reply'].click();
  }
});
new ResizeObserver(fitMessage).observe(elements['composer-container']);

const scroller = elements.conversation;
let timelineHeight = scroller.clientHeight;
let timelineWidth = scroller.clientWidth;
scroller.classList.add('follow-newest');
scroller.addEventListener('scroll', () => {
  updateJumpToLatest();
  // A resize can also scroll the timeline. Only update the reading position at the settled size.
  if (scroller.clientHeight === timelineHeight && scroller.clientWidth === timelineWidth) {
    scroller.classList.toggle('follow-newest', timelineAtEnd(scroller, timelineHeight));
  }
}, { passive: true });
new ResizeObserver(() => {
  const height = scroller.clientHeight;
  const width = scroller.clientWidth;
  const widthChanged = width !== timelineWidth;
  const resized = height !== timelineHeight || widthChanged;
  // At the same width, a scroll can already have moved away from the previous bottom.
  const wasAtBottom = widthChanged || timelineAtEnd(scroller, timelineHeight);
  timelineHeight = height;
  timelineWidth = width;
  if (width > 0 && resized && wasAtBottom && scroller.classList.contains('follow-newest')) scroller.scrollTop = scroller.scrollHeight;
  updateJumpToLatest();
}).observe(scroller);
// The timeline grows without resizing the scroller, which changes the history fades.
new ResizeObserver(updateJumpToLatest).observe(elements.timeline);

const visualViewport = window.visualViewport;
if (visualViewport) {
  const touchInput = matchMedia('(any-pointer: coarse)');
  let viewportFrame = null;
  const updateKeyboardViewport = () => {
    if (viewportFrame !== null) return;
    viewportFrame = requestAnimationFrame(() => {
      viewportFrame = null;
      const height = Math.floor(visualViewport.height);
      // Leave browser chrome and pinch zoom alone. Use the fallback only when the keyboard shrinks the visual viewport.
      const keyboardOverlaysLayout = touchInput.matches && document.activeElement === elements.message &&
        !elements.message.readOnly && Math.abs(visualViewport.scale - 1) < .01 && window.innerHeight - height > 80;
      if (keyboardOverlaysLayout) {
        workspace.style.setProperty('--keyboard-viewport-height', `${height}px`);
        workspace.style.setProperty('--keyboard-viewport-top', `${visualViewport.offsetTop}px`);
      } else {
        workspace.style.removeProperty('--keyboard-viewport-height');
        workspace.style.removeProperty('--keyboard-viewport-top');
      }
      workspace.classList.toggle('keyboard-viewport', keyboardOverlaysLayout);
    });
  };
  visualViewport.addEventListener('resize', updateKeyboardViewport);
  visualViewport.addEventListener('scroll', updateKeyboardViewport);
  window.addEventListener('resize', updateKeyboardViewport);
  elements.message.addEventListener('focus', updateKeyboardViewport);
  elements.message.addEventListener('blur', updateKeyboardViewport);
}
document.addEventListener('click', (event) => {
  if (menu && !event.target.closest('.row-menu, .menu-popover')) closeMenu(false);
  if (!conversationActions.contains(event.target)) closeConversationActions();
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
  if (event.key !== 'Escape' || event.defaultPrevented || event.isComposing || event.keyCode === 229) return;
  if (conversationActions.open) closeConversationActions(true);
  else if (menu) closeMenu();
  else if (workspace.classList.contains('nav-open') && !elements.detail.open && !elements['bot-editor'].open) openNavigation(false);
  else if (view?.dm && !elements.detail.open && !elements['bot-editor'].open && !decisions.isOpen()) {
    event.preventDefault();
    leavePeerConversation();
  }
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

let sessionDateRefresh;
function refreshSessionDate() {
  clearTimeout(sessionDateRefresh);
  if (document.hidden) return;
  const now = new Date();
  const midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1);
  sessionDateRefresh = setTimeout(refreshSessionDate, midnight - now);
  renderActivity();
}

function resumeUpdates() {
  refreshSessionDate();
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
  clearTimeout(sessionDateRefresh);
  liveUpdates.use(null);
});
window.addEventListener('pageshow', (event) => { if (event.persisted) resumeUpdates(); });
window.addEventListener('popstate', () => { if (listLoaded) selectLocation(); });
