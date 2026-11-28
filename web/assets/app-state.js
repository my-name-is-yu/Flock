import { createComposerDraftStore } from './composer-drafts.mjs';

export const byId = (id) => document.getElementById(id);
export const elements = Object.fromEntries([
  'navigation', 'pinned', 'bot-list', 'wait-list', 'wait-count', 'session-list', 'more-sessions',
  'connection', 'connection-text', 'new-bot-button', 'open-navigation', 'close-navigation', 'profile-avatar', 'profile-avatar-button',
  'profile-name', 'profile-status', 'profile-identity', 'profile-toolbar', 'profile-placeholder', 'conversation', 'older',
  'timeline', 'conversation-source-error', 'jump-to-latest', 'composer-container', 'composer', 'reply-context', 'reply-summary', 'cancel-reply',
  'decision-reply-context', 'decision-reply-label', 'decision-reply-target',
  'reply-label', 'compose-label', 'message', 'composer-ghost', 'send', 'notice', 'view-only-notice', 'detail', 'detail-title', 'detail-body', 'close-detail',
  'composer-banner', 'composer-banner-title', 'composer-banner-description', 'composer-banner-action',
  'attach-screenshots', 'screenshot-input', 'screenshot-previews',
  'bot-editor', 'bot-activity', 'activity-scope-name',
  'sidebar-resizer',
].map((id) => [id, byId(id)]));
export const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
export const motionAnimations = new WeakMap();
export const motionEaseOut = 'cubic-bezier(0.23, 1, 0.32, 1)';
export const workspace = document.querySelector('.workspace');
export const conversationActions = byId('conversation-actions');
export const conversationActionsTrigger = conversationActions.querySelector('summary');
export const narrow = matchMedia('(max-width: 700px)');
export const noHover = matchMedia('(hover: none)');
export const dayFormat = new Intl.DateTimeFormat('en', { year: 'numeric', month: 'long', day: 'numeric' });
export const fullTime = new Intl.DateTimeFormat('en', { dateStyle: 'medium', timeStyle: 'short', hourCycle: 'h23' });
export const byteLength = (text) => new TextEncoder().encode(text).length;
export const composerDrafts = createComposerDraftStore();
export const screenshotDrafts = new WeakMap();
export const sendingMessages = new Set();
export const notices = new Map();
export const signatures = new Map();
export let bots = [];
export let rosterRevision = 0;
export let listLoaded = false;
export let view = null;
export let visibilityGeneration = 0;
export let menu = null;
export const responseViewers = new WeakMap();
export const botQuery = (path, bot, before) => `${path}?${new URLSearchParams(before ? { bot, before } : { bot })}`;
export const showingComposer = (bot) => view?.id === bot;
export const navBackdrop = byId('nav-backdrop');

export const sidebarIcons = {
  pause: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" aria-hidden="true"><path d="M9 5v14M15 5v14"/></svg>',
  resume: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round" aria-hidden="true"><path d="m8 5 11 7-11 7Z"/></svg>',
  trash: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 6h16M9 6V4h6v2M6 6l1 14h10l1-14M10 10v6m4-6v6"/></svg>',
  more: '<svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><circle cx="6" cy="12" r="1.6"/><circle cx="12" cy="12" r="1.6"/><circle cx="18" cy="12" r="1.6"/></svg>',
  pin: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 4h6l-1 5 3 3v2H7v-2l3-3-1-5Z"/><path d="M12 14v6"/></svg>',
  inbox: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 13.5 6.2 6a2 2 0 0 1 1.9-1.5h7.8A2 2 0 0 1 17.8 6l2.2 7.5V18a1.5 1.5 0 0 1-1.5 1.5h-13A1.5 1.5 0 0 1 4 18v-4.5Z"/><path d="M4 13.5h4.5l1 2h5l1-2H20"/></svg>',
  settings: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 7h3m4 0h9M4 12h9m4 0h3M4 17h3m4 0h9"/><circle cx="9" cy="7" r="2"/><circle cx="15" cy="12" r="2"/><circle cx="9" cy="17" r="2"/></svg>',
  open: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M8 16 16 8M10 8h6v6"/></svg>',
  stop: '<svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><rect x="6.5" y="6.5" width="11" height="11" rx="2"/></svg>',
};
export const sidebar = byId('navigation');
export let readState = new Map();
export let sidebarViewer = null;

export let botDraft = null;
export let settingsReturnFocus = null;

export let messageTap = null;
export const tapMoved = (event, tap) => Math.hypot(event.clientX - tap.x, event.clientY - tap.y) > 10;
export const hasTextSelection = () => window.getSelection()?.isCollapsed === false;
export const composerLimit = (limits, reply, decision) =>
  decision || reply?.kind === 'wait' ? limits.waitAnswerBytes : limits.messageBytes;
export const screenshotAnchor = (reply) => reply?.messageId ?? 0;
export const screenshotsFor = (composer) => screenshotDrafts.get(composer);
export let pickerTarget = null;

export const closedQuestion = (context, reply) => reply?.kind === 'wait' &&
  !context.data.waits.some((task) => task.wait.messageId === reply.messageId);
export let composition = null;

export const detailNav = byId('detail-nav');
export let detailOrigin = null;
export let reopenSettingsOnClose = false;
export let finishingBotEditor = false;
export let notificationDestination;

export const findPanel = byId('conversation-find');
export const sidebarToggle = byId('toggle-chat-sidebar');
export const sidebarShow = byId('show-chat-sidebar');
export const sidebarResizer = elements['sidebar-resizer'];
export const sidebarMinWidth = 216;
export const sidebarMaxWidth = 416;
export const sidebarDefaultWidth = 296;
export const sidebarCollapseThreshold = 160;
export let dragDepth = 0;

export const scroller = elements.conversation;
export let timelineHeight = scroller.clientHeight;
export let timelineWidth = scroller.clientWidth;

export const visualViewport = window.visualViewport;

export let syncRetry;

export let sessionDateRefresh;

export function assignMenu(value) {
  menu = value;
}

export function assignView(value) {
  view = value;
}

export function assignSettingsReturnFocus(value) {
  settingsReturnFocus = value;
}

export function assignMessageTap(value) {
  messageTap = value;
}

export function assignTimelineHeight(value) {
  timelineHeight = value;
}

export function assignTimelineWidth(value) {
  timelineWidth = value;
}

export function assignDragDepth(value) {
  dragDepth = value;
}

export function assignComposition(value) {
  composition = value;
}

export function assignDetailOrigin(value) {
  detailOrigin = value;
}

export function assignNotificationDestination(value) {
  notificationDestination = value;
}

export function assignBotDraft(value) {
  botDraft = value;
}

export function assignReopenSettingsOnClose(value) {
  reopenSettingsOnClose = value;
}

export function assignFinishingBotEditor(value) {
  finishingBotEditor = value;
}

export function assignRosterRevision(value) {
  rosterRevision = value;
}

export function assignBots(value) {
  bots = value;
}

export function assignSidebarViewer(value) {
  sidebarViewer = value;
}

export function assignReadState(value) {
  readState = value;
}

export function assignListLoaded(value) {
  listLoaded = value;
}

export function assignPickerTarget(value) {
  pickerTarget = value;
}

export function assignSyncRetry(value) {
  syncRetry = value;
}

export function assignSessionDateRefresh(value) {
  sessionDateRefresh = value;
}

export function assignVisibilityGeneration(value) {
  visibilityGeneration = value;
}
