import './tooltip.js';
import { mountBoardInbox, taskWaitDecision } from './board-inbox.js';
import { mountBotDeletion } from './bot-deletion.js';
import { createBotEditor } from './bot-editor.js';
import { mountBotPause } from './bot-pause.js';
import { browserNotificationsEnabled, createBrowserNotifications, onNotificationPreferenceChange } from './browser-notifications.mjs';
import { messageSide } from './chat-grouping.mjs';
import { attachmentMessageBody, screenshotMessage } from './composer-screenshots.mjs';
import { handleComposerEnter, isComposerSuggestionKey } from './composer-shortcuts.mjs';
import { decisionConfirmations, responseErrors } from './conversation-copy.mjs';
import { mountConversationFind } from './conversation-find.mjs';
import { enableBackdropDismiss } from './dialog-backdrop.mjs';
import { refreshEventHistory } from './event-history.mjs';
import { coalescedRefresh, createLiveUpdates } from './live-updates.js';
import { closeConversationAttachments } from './message-attachments.js';
import {
  assignBotDeletion,
  assignBotDraft,
  assignBotEditor,
  assignBotPause,
  assignBots,
  assignCloseDetail,
  assignComposition,
  assignConnection,
  assignConversationFind,
  assignDecisions,
  assignDetailOrigin,
  assignDragDepth,
  assignFinishingBotEditor,
  assignHeaderFocusTarget,
  assignListLoaded,
  assignLiveUpdates,
  assignMenu,
  assignMessageTap,
  assignNotificationDestination,
  assignOpenDetail,
  assignOpenNewBot,
  assignOperate,
  assignPickerTarget,
  assignReadState,
  assignRefreshBots,
  assignRender,
  assignRenderComposer,
  assignReopenSettingsOnClose,
  assignRosterRevision,
  assignSelectBot,
  assignSessionDateRefresh,
  assignSetNotice,
  assignSettingsReturnFocus,
  assignSidebarViewer,
  assignSyncRetry,
  assignTimelineHeight,
  assignTimelineWidth,
  assignView,
  assignVisibilityGeneration,
  botQuery,
  bots,
  byId,
  byteLength,
  closedQuestion,
  composerDrafts,
  composerLimit,
  composition,
  connection,
  conversationActions,
  conversationActionsTrigger,
  detailNav,
  detailOrigin,
  dragDepth,
  elements,
  findPanel,
  hasTextSelection,
  listLoaded,
  menu,
  messageTap,
  narrow,
  navBackdrop,
  noHover,
  notices,
  pickerTarget,
  readState,
  reducedMotion,
  reopenSettingsOnClose,
  responseViewers,
  rosterRevision,
  screenshotAnchor,
  screenshotsFor,
  scroller,
  sendingMessages,
  sessionDateRefresh,
  settingsReturnFocus,
  showingComposer,
  sidebar,
  sidebarCollapseThreshold,
  sidebarDefaultWidth,
  sidebarMaxWidth,
  sidebarMinWidth,
  sidebarResizer,
  sidebarShow,
  sidebarToggle,
  sidebarViewer,
  signatures,
  syncRetry,
  tapMoved,
  timelineHeight,
  timelineWidth,
  view,
  visibilityGeneration,
  visualViewport,
  workspace,
} from './app-state.js';
import { animateViewTransition, api, closeConversationActions, conversationUrl, openNavigation, pins, save, saved } from './app-core.js';
import {
  closeMenu,
  focusKey,
  openBotSettings,
  openSettingsFromMenu,
  pinnedBotsInShortcutOrder,
  renderBots,
  scheduleReadCheck,
} from './sidebar.js';
import { renderActivity, renderHeader } from './activity.js';
import {
  backToLatest,
  beginReply,
  cardSourceAction,
  closeMessageTime,
  eventHistoryForRefresh,
  leaveConversationSource,
  messageBubble,
  nextSuggestion,
  rememberReadingPosition,
  renderTimeline,
  resetConversationJump,
  showConversationMessage,
  syncMessageFolding,
  timelineAtEnd,
  timelineScrollPosition,
  updateFindOverlap,
  updateJumpToLatest,
} from './timeline.js';
import { addScreenshots, clearScreenshots, fitMessage, renderComposer, setNotice } from './composer.js';
import {
  clearDetailSectionTransition,
  closeDetail,
  detail,
  detailWorkspace,
  focusAfterBotDeletion,
  followNotification,
  headerFocusTarget,
  openDetail,
  openNewBot,
  releaseDetailAvatar,
  renderDetail,
  selectSection,
  setBotPaused,
  setDetail,
  showDetailError,
} from './bot-detail.js';

// The parts of the page reach these through app-state.js, so they do not import this script.
assignOpenDetail(openDetail);
assignSelectBot(selectBot);
assignCloseDetail(closeDetail);
assignRenderComposer(renderComposer);
assignSetNotice(setNotice);
assignOperate(operate);
assignRender(render);
assignOpenNewBot(openNewBot);
assignHeaderFocusTarget(headerFocusTarget);


function setConnection(state) {
  if (connection === state) return;
  assignConnection(state);
  elements.connection.dataset.state = state;
  elements['connection-text'].textContent = state === 'offline' ? 'Can’t reach Flock. Reconnecting…' : '';
  renderHeader();
}
function selectBot(botId, { updateHistory = true } = {}) {
  assignMenu(null);
  closeConversationActions();
  openNavigation(false);
  if (view?.id === botId) {
    if (view.source || view.pendingSource) backToLatest();
    else renderBots();
    return;
  }
  const prevBotId = view?.id ?? null;
  resetConversationJump(view);
  rememberReadingPosition();
  closeDetail();
  closeConversationAttachments();
  const existing = view?.id === botId ? view : null;
  assignView(botId ? { id: botId, data: null, events: new Map(), olderBefore: 0, loadedSessions: new Map(),
    sessionsBefore: 0, ...existing, source: null, pendingSource: null, loadingOlder: false, loadingSessions: false } : null);
  const url = conversationUrl(botId);
  if (url.href !== location.href) {
    if (updateHistory) history.pushState(null, '', url);
    else history.replaceState(null, '', url);
  }
  save('flock-selected-bot', botId);
  render('bottom');
  if (prevBotId && prevBotId !== botId) animateViewTransition('bot-switch');
  const context = view;
  if (context) {
    context.pageRead = Promise.resolve();
    context.refresh = coalescedRefresh((scroll, animationGeneration) =>
      readTimeline(context, () => refreshView(context, scroll, animationGeneration)));
    void context.refresh(context.data ? undefined : 'bottom');
  }
  subscribeToView();
}
function selectLocation() {
  const params = new URLSearchParams(location.search);
  const requested = params.get('bot');
  const bot = bots.find((entry) => entry.id === requested);
  const wantsSettings = params.get('settings') === '1';
  const preferred = saved('flock-selected-bot', null);
  const selectedBot = bot?.id ?? bots.find((entry) => entry.id === preferred)?.id ?? bots[0]?.id ?? null;
  selectBot(selectedBot, { updateHistory: false });
  if (wantsSettings) {
    const url = conversationUrl(selectedBot);
    url.searchParams.delete('settings');
    history.replaceState(history.state, '', url);
    if (bot && view?.id === bot.id) openBotSettings(bot.id);
  }
}

// Observe the native click after a short, stationary press. Leave scrolling and selection to the browser.
elements.timeline.addEventListener('pointerdown', (event) => {
  const bubble = noHover.matches && event.isPrimary && event.button === 0 && messageBubble(event.target);
  assignMessageTap(bubble && !hasTextSelection()
    ? { bubble, id: event.pointerId, x: event.clientX, y: event.clientY, start: event.timeStamp, released: false }
    : null);
}, { passive: true });
elements.timeline.addEventListener('pointermove', (event) => {
  if (messageTap?.id === event.pointerId && tapMoved(event, messageTap)) assignMessageTap(null);
}, { passive: true });
elements.timeline.addEventListener('pointerup', (event) => {
  if (messageTap?.id !== event.pointerId) return;
  if (tapMoved(event, messageTap) || event.timeStamp - messageTap.start >= 500) assignMessageTap(null);
  else messageTap.released = true;
}, { passive: true });
for (const type of ['pointercancel', 'contextmenu']) {
  elements.timeline.addEventListener(type, () => { assignMessageTap(null); }, { passive: true });
}
elements.timeline.addEventListener('click', (event) => {
  const tap = messageTap;
  assignMessageTap(null);
  const context = view;
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
  assignMessageTap(null);
  if (!noHover.matches) closeMessageTime();
});
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
  const origin = detailOrigin;
  assignDetailOrigin(null);
  const active = document.activeElement;
  if (origin?.isConnected && (active === document.body || elements.detail.contains(active))) origin.focus();
});
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

const botEditor = createBotEditor(elements['bot-editor'], {
  api,
  onDraft(draft) {
    assignBotDraft(draft);
    renderBots();
  },
  async onSaved(savedBot, savedMode) {
    const returnToSettings = reopenSettingsOnClose;
    assignReopenSettingsOnClose(false);
    assignFinishingBotEditor(true);
    try {
      await Promise.all([refreshBots(), view?.id === savedBot.id && view.refresh()]);
    } finally {
      assignFinishingBotEditor(false);
    }
    if (followNotification()) return;
    if (savedMode === 'create') {
      selectBot(savedBot.id);
      return;
    }
    if (returnToSettings && view?.id === savedBot.id && view.data) {
      openDetail({ kind: 'settings', section: 'general', status: 'Appearance saved.' });
    }
  },
  onCancel(savedMode) {
    const returnToSettings = reopenSettingsOnClose;
    assignReopenSettingsOnClose(false);
    if (followNotification()) return;
    if (savedMode === 'edit' && returnToSettings && view?.data) {
      openDetail({ kind: 'settings', section: 'general' });
    }
  },
});
assignBotEditor(botEditor);

const botDeletion = mountBotDeletion(byId('bot-delete'), {
  remove: (botId) => api('bots/delete', { botId }),
  focusFallback: focusAfterBotDeletion,
  onDeleted(id) {
    if (pins.delete(id)) save('flock-pins', [...pins]);
    if (settingsReturnFocus === id) assignSettingsReturnFocus(null);
    assignRosterRevision(rosterRevision + 1);
    assignBots(bots.filter((bot) => bot.id !== id));
    readState.delete(id);
    if (view?.id === id) selectLocation();
    else render();
    void refreshBots();
  },
});
assignBotDeletion(botDeletion);
const botPause = mountBotPause(byId('bot-pause'), setBotPaused);
assignBotPause(botPause);

elements['composer-banner-action'].addEventListener('click', async (event) => {
  const control = event.currentTarget;
  const context = view;
  const bot = context?.data?.bot;
  if (!bot?.canDelete) return;
  if (!bot.paused && !bot.model) {
    openDetail({ kind: 'settings', section: 'general' });
    detailWorkspace?.edit();
    return;
  }
  if (!bot.paused) return;
  control.disabled = true;
  context.resumeError = '';
  renderComposer();
  try {
    await setBotPaused(bot, false);
    if (view === context && !elements.composer.hidden) elements.message.focus({ preventScroll: true });
  } catch {
    context.resumeError = 'Couldn’t resume this Bot. Try again.';
    if (view === context) renderComposer();
  } finally {
    control.disabled = false;
    if (view === context && !control.hidden) control.focus({ preventScroll: true });
  }
});
function render(scroll, liveAfterId) {
  decisions.setContext(view?.id, sidebarViewer);
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
  if (!latest.bot.paused) delete context.resumeError;
  const composer = context.composer = latest.canDecide === true && !latest.bot.paused
    ? composerDrafts(context.id, latest.composerPerson) : null;
  if (!composer) return;
  if (!sendingMessages.has(composer) && closedQuestion(context, composer.reply) && !composer.text &&
      !screenshotsFor(composer)?.get(screenshotAnchor(composer.reply))?.images.length) {
    composer.closeReply();
    setNotice(context, 'That question is no longer open, so your reply wasn’t sent.');
  }
}
async function conversationPage(timeline, before) {
  return api(botQuery('conversation', timeline.id, before));
}
async function refreshView(context, scroll, animationGeneration) {
  const isCurrent = () => context === view &&
    (animationGeneration == null || (animationGeneration === visibilityGeneration && !document.hidden));
  if (!isCurrent()) return;
  const liveAfterId = animationGeneration == null ? undefined : context.newestEvent;
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
  const revisionAtFetch = rosterRevision;
  try {
    const latest = await api('bots');
    if (revisionAtFetch !== rosterRevision) return;
    const viewer = responseViewers.get(latest) ?? null;
    const previous = viewer === sidebarViewer ? readState : null;
    assignSidebarViewer(viewer);
    assignReadState(new Map());
    // The first successful list sets a baseline for existing conversations in this tab.
    assignBots(latest.map(({ latestIncomingEventId: incoming, ...bot }) => {
      if (viewer !== null) {
        const readThrough = previous?.get(bot.id)?.readThrough ?? incoming;
        readState.set(bot.id, { incoming, readThrough });
      }
      return bot;
    }));
    assignListLoaded(true);
    setConnection('ready');
    if (!bots.some((bot) => bot.id === view?.id)) selectLocation();
    render();
    await decisions.refresh();
  } catch (error) {
    if (revisionAtFetch !== rosterRevision) return;
    if (error.status === 401 || error.status === 403) {
      assignSidebarViewer(null);
      decisions.setContext(null, null);
      render();
    }
    setConnection('offline');
    retrySync();
  }
});
assignRefreshBots(refreshBots);
function readTimeline(context, read) {
  const next = (context.pageRead ?? Promise.resolve()).then(read, read);
  context.pageRead = next;
  return next;
}

async function loadOlder() {
  const context = view;
  if (!context?.olderBefore || context.loadingOlder) return;
  context.loadingOlder = true;
  renderTimeline();
  try {
    await readTimeline(context, async () => {
      if (context !== view || !context.olderBefore) return;
      const cursor = context.olderBefore;
      const { events, olderBefore } = await conversationPage(context, cursor);
      if (context !== view || context.olderBefore !== cursor) return;
      context.olderBefore = olderBefore;
      for (const event of events) context.events.set(event.id, event);
      renderTimeline('older');
    });
  } catch {
    if (context !== view) return;
    setNotice(context, 'Couldn’t load earlier messages.', true);
  }
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
    mergeSessionTasks(context, page.sessionTasks);
    context.sessionsBefore = page.sessionsBefore;
  } catch { if (context === view) setNotice(context, 'Couldn’t load earlier requests.', true); }
  finally {
    context.loadingSessions = false;
    if (context === view) renderActivity();
  }
}

function operate(context, task, operation, data) {
  const current = context.data?.waits.find((entry) => entry.id === task.id && entry.wait.id === task.wait.id);
  return submitConversationDecision(context, taskWaitDecision(context.id, task),
    current && taskWaitDecision(context.id, current), operation, data);
}
function submitConversationDecision(context, item, active, operation, data) {
  if (view !== context || !context.composer ||
      (responseViewers.get(context.data) ?? null) !== sidebarViewer || !active) return;
  const selection = detail;
  const isOpen = () => !item.itemKind && view === context && detail === selection &&
    detail?.taskId === item.taskId && detail?.waitId === item.waitId;
  return decisions.submitConversation(item, operation, data, {
    active: { ...active, canDecide: true },
    start: () => setNotice(context, 'Submitting action…'),
    finish(error, refreshInbox) {
      if (error) {
        const message = responseErrors[error.message] ?? 'Flock could not confirm the action. Check the conversation.';
        setNotice(context, message, true);
        if (isOpen()) showDetailError(message);
      } else {
        setNotice(context, operation === 'answer' ? 'Answer received.' : 'Action received.');
        if (isOpen()) closeDetail();
      }
      if (view?.id === context.id) {
        return Promise.all([view.refresh(), error || !liveUpdates.live ? refreshBots() : refreshInbox()]);
      }
      return refreshInbox();
    },
  });
}
async function submit(event) {
  event.preventDefault();
  const context = view;
  if (!context?.composer || elements.send.disabled) return;
  if (decisions.composer()) return decisions.submitAnswer();
  if (leaveConversationSource(context)) {
    render('bottom');
  }
  const { composer } = context;
  const reply = composer.reply;
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
    await api('messages', attachments.length ? attachmentMessageBody(payload) : payload);
    if (attachments.length) {
      composer.settleScreenshotSend(payload);
      clearScreenshots(composer, reply);
    } else composer.settleSend(payload, true);
    setNotice(context, 'Message received.', false, { quiet: true });
    if (showingComposer(context.id)) elements.conversation.scrollTop = elements.conversation.scrollHeight;
    if (!liveUpdates.live) await Promise.all([refreshBots(), showingComposer(context.id) && view.refresh('bottom')]);
  } catch (error) {
    context.pendingSentAfterId = null;
    if (payload && error.status >= 400 && error.status < 500) {
      if (attachments.length) delete screenshots.pending;
      else composer.settleSend(payload, false);
    }
    if (error.message === 'bot_paused') void Promise.all([refreshBots(), context.refresh()]);
    const notice = ['removed_bot_setting', 'invalid_attachment'].includes(error.message) ? error.data.message : responseErrors[error.message];
    setNotice(context, notice ?? 'Flock could not confirm receipt. You can resend the same message.', true);
  }
  finally {
    sendingMessages.delete(composer);
    if (showingComposer(context.id)) renderComposer();
  }
}
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
assignConversationFind(conversationFind);
new ResizeObserver(updateFindOverlap).observe(findPanel);

const decisions = mountBoardInbox({
  api,
  sourceAction: cardSourceAction,
  onRefreshError: retrySync,
  refreshConversation(item) {
    if (view && (view.id === item.botId || view.id === item.conversationBotId)) return view.refresh();
  },
  canCompose: () => !!view?.composer && responseViewers.get(view.data) === sidebarViewer,
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
    renderDetail();
    scheduleReadCheck();
  },
  onAccepted(item, { decision, error }) {
    if (!view || (view.id !== item.botId && view.id !== item.conversationBotId)) return;
    const name = bots.find((bot) => bot.id === item.botId)?.name ?? 'The Bot';
    setNotice(view, error ?? decisionConfirmations[decision]?.(name) ?? 'Response sent.', !!error);
  },
});
assignDecisions(decisions);

elements['new-bot-button'].addEventListener('click', () => openNewBot(elements['new-bot-button']));
elements['open-navigation'].addEventListener('click', () => openNavigation(true));
elements['close-navigation'].addEventListener('click', () => openNavigation(false));
navBackdrop?.addEventListener('click', () => openNavigation(false));
narrow.addEventListener('change', () => openNavigation(false, false));
sidebarResizer?.setAttribute('aria-valuemin', String(sidebarMinWidth));
sidebarResizer?.setAttribute('aria-valuemax', String(sidebarMaxWidth));
function setSidebarWidth(px, persist = false) {
  const clamped = Math.round(Math.max(sidebarMinWidth, Math.min(sidebarMaxWidth, px)));
  workspace.style.setProperty('--sidebar-width', `${clamped}px`);
  sidebarResizer?.setAttribute('aria-valuenow', String(clamped));
  if (persist) save('flock-sidebar-width', clamped);
}
function setSidebarCollapsed(collapsed, persist = true) {
  const moveFocus = document.activeElement === (collapsed ? sidebarToggle : sidebarShow);
  workspace.classList.toggle('sidebar-collapsed', collapsed);
  sidebarToggle.setAttribute('aria-expanded', String(!collapsed));
  sidebarShow.hidden = !collapsed;
  if (sidebarResizer) {
    sidebarResizer.tabIndex = collapsed ? -1 : 0;
    sidebarResizer.setAttribute('aria-hidden', String(collapsed));
  }
  if (persist) save('flock-sidebar-collapsed', collapsed);
  if (moveFocus) (collapsed ? sidebarShow : sidebarToggle).focus({ preventScroll: true });
}
const storedSidebarWidth = saved('flock-sidebar-width', null);
if (Number.isFinite(storedSidebarWidth) && storedSidebarWidth >= sidebarMinWidth && storedSidebarWidth <= sidebarMaxWidth) {
  setSidebarWidth(storedSidebarWidth, false);
} else {
  sidebarResizer?.setAttribute('aria-valuenow', String(sidebarDefaultWidth));
}
if (saved('flock-sidebar-collapsed', false)) {
  setSidebarCollapsed(true, false);
}
sidebarToggle.addEventListener('click', () => setSidebarCollapsed(true));
sidebarShow.addEventListener('click', () => setSidebarCollapsed(false));
if (sidebarResizer) {
  let dragPointer = null;
  let lastWidth = Number.isFinite(storedSidebarWidth) && storedSidebarWidth >= sidebarMinWidth && storedSidebarWidth <= sidebarMaxWidth
    ? storedSidebarWidth : sidebarDefaultWidth;
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
  const resetSidebarWidth = () => {
    workspace.style.removeProperty('--sidebar-width');
    save('flock-sidebar-width', null);
    lastWidth = sidebarDefaultWidth;
    sidebarResizer.setAttribute('aria-valuenow', String(sidebarDefaultWidth));
    setSidebarCollapsed(false);
  };
  sidebarResizer.addEventListener('dblclick', resetSidebarWidth);
  sidebarResizer.addEventListener('keydown', (event) => {
    if (narrow.matches) return;
    let nextWidth = null;
    if (event.key === 'ArrowLeft') nextWidth = lastWidth - 16;
    else if (event.key === 'ArrowRight') nextWidth = lastWidth + 16;
    else if (event.key === 'Home') nextWidth = sidebarMinWidth;
    else if (event.key === 'End') nextWidth = sidebarMaxWidth;
    else if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      resetSidebarWidth();
      return;
    } else return;
    event.preventDefault();
    lastWidth = Math.max(sidebarMinWidth, Math.min(sidebarMaxWidth, nextWidth));
    if (workspace.classList.contains('sidebar-collapsed')) setSidebarCollapsed(false);
    setSidebarWidth(lastWidth, true);
  });
}
function openCurrentBotSettings() {
  closeConversationActions();
  openDetail({ kind: 'settings' });
}
elements['profile-avatar-button'].addEventListener('click', openCurrentBotSettings);
byId('conversation-bot-settings')?.addEventListener('click', openCurrentBotSettings);
byId('profile-heading')?.addEventListener('click', (event) => {
  if (byId('needs-you')?.hidden && !event.target.closest('button')) openCurrentBotSettings();
});
byId('conversation-recent-decisions').addEventListener('click', () => {
  closeConversationActions();
  void decisions.openHistory(conversationActionsTrigger);
});
elements.conversation.addEventListener('click', (event) => {
  if (window.getSelection?.()?.toString()) return;
  if (event.target.closest('.bubble, button, a, input, textarea, summary, .msg-actions, .msg-meta, .older, .empty-state')) return;
  if (!findPanel.hidden) conversationFind.close(false);
});
elements['close-detail'].addEventListener('click', closeDetail);
enableBackdropDismiss(elements.detail, {
  canDismiss: () => {
    const keep = elements.detail.querySelector('.wait-stop-keep');
    if (keep) {
      if (!keep.disabled) keep.click();
      return false;
    }
    return true;
  },
  close: closeDetail,
});
elements.detail.addEventListener('close', () => {
  if (elements.detail.open || !settingsReturnFocus) return;
  if (narrow.matches) openNavigation(true, false);
  focusKey(`menu:${settingsReturnFocus}`);
  assignSettingsReturnFocus(null);
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
  assignPickerTarget({ context: view, composer: view.composer, reply: view.composer.reply });
  elements['screenshot-input'].click();
});
elements['screenshot-input'].addEventListener('change', (event) => {
  const files = Array.from(event.target.files);
  event.target.value = '';
  const target = pickerTarget;
  assignPickerTarget(null);
  if (target) void addScreenshots(target.context, target.composer, target.reply, files);
});
elements['screenshot-input'].addEventListener('cancel', () => { assignPickerTarget(null); });
elements.message.addEventListener('paste', (event) => {
  const files = Array.from(event.clipboardData?.files ?? []);
  if (!files.length || elements.message.readOnly || decisions.composer()) return;
  event.preventDefault();
  void addScreenshots(view, view.composer, view.composer.reply, files);
});
const canDropScreenshots = (event) => !elements.message.readOnly && !decisions.composer() &&
  Array.from(event.dataTransfer?.types ?? []).includes('Files');
elements.composer.addEventListener('dragenter', (event) => {
  if (!canDropScreenshots(event)) return;
  event.preventDefault();
  assignDragDepth(dragDepth + 1);
  elements.composer.classList.add('dragover');
});
elements.composer.addEventListener('dragover', (event) => {
  if (!canDropScreenshots(event)) return;
  event.preventDefault();
  if (event.dataTransfer) event.dataTransfer.dropEffect = 'copy';
  elements.composer.classList.add('dragover');
});
elements.composer.addEventListener('dragleave', () => {
  if (dragDepth > 0) assignDragDepth(dragDepth - 1);
  if (!dragDepth) elements.composer.classList.remove('dragover');
});
elements.composer.addEventListener('drop', (event) => {
  assignDragDepth(0);
  elements.composer.classList.remove('dragover');
  if (!canDropScreenshots(event)) return;
  event.preventDefault();
  const files = Array.from(event.dataTransfer?.files ?? []);
  if (files.length) void addScreenshots(view, view.composer, view.composer.reply, files);
});
function updateDraft() {
  if (!view?.composer || elements.message.readOnly) return;
  const decision = decisions.composer();
  const reply = view.composer.reply;
  if (decision) decisions.edit(elements.message.value);
  else view.composer.edit(elements.message.value, reply);
  fitMessage();
  notices.delete(view.id);
  const limit = composerLimit(view.data.limits, reply, decision);
  if (byteLength(elements.message.value) > limit) setNotice(view, `Messages can be up to ${limit.toLocaleString('en')} bytes.`, true);
  else renderComposer();
}
elements.message.addEventListener('compositionstart', () => {
  if (!view?.composer || elements.message.readOnly) return;
  assignComposition({ composer: view.composer, anchor: decisions.composer()?.key ?? view.composer.reply?.messageId });
  renderComposer();
});
elements.message.addEventListener('compositionend', () => {
  if (!composition) return;
  assignComposition(null);
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
  const fromMessage = event.target === elements.message;
  const canAcceptSuggestion = fromMessage && !elements.message.readOnly && !elements.message.value &&
    !elements['composer-ghost'].hidden;
  if (canAcceptSuggestion && isComposerSuggestionKey(event)) {
    const suggestion = nextSuggestion(view);
    if (suggestion) {
      event.preventDefault();
      beginReply(suggestion.target, suggestion.text);
      return;
    }
  }
  if (fromMessage) handleComposerEnter(event, elements.send);
  if (event.key === 'Escape' && !elements['reply-context'].hidden) {
    event.preventDefault();
    event.stopPropagation();
    elements['cancel-reply'].click();
  }
});
new ResizeObserver(fitMessage).observe(elements['composer-container']);
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
  assignTimelineHeight(height);
  assignTimelineWidth(width);
  if (width > 0 && widthChanged) syncMessageFolding([...elements.timeline.children]);
  if (width > 0 && resized && wasAtBottom && scroller.classList.contains('follow-newest')) scroller.scrollTop = scroller.scrollHeight;
  updateJumpToLatest();
}).observe(scroller);
// The timeline grows without resizing the scroller, which changes the history fades.
new ResizeObserver(updateJumpToLatest).observe(elements.timeline);
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
sidebar.querySelector('.sidebar-scroll')?.addEventListener('scroll', () => { if (menu) closeMenu(false); }, { passive: true });
window.addEventListener('resize', () => { if (menu) closeMenu(false); });
document.addEventListener('keydown', (event) => {
  if (botDeletion.isOpen() || botPause.isOpen()) return;
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
});
const browserNotifications = createBrowserNotifications({
  viewingBot: () => view?.id ?? null,
  openBot(botId) {
    assignNotificationDestination(botId);
    followNotification();
  },
});
function retrySync() {
  if (!document.hidden) assignSyncRetry(syncRetry ?? (setTimeout(catchUp, 2500)));
}

function catchUp() {
  clearTimeout(syncRetry);
  assignSyncRetry(null);
  void browserNotifications.refresh();
  if (document.hidden) return;
  void refreshBots();
  void view?.refresh();
}

const liveUpdates = createLiveUpdates(catchUp, (change) => {
  if (change.list === true) void browserNotifications.refresh();
  if (document.hidden) return;
  if (change.list === true) void refreshBots();
  if (view && change.bot === view.id) void view.refresh(undefined, liveUpdates.live ? visibilityGeneration : null);
});
assignLiveUpdates(liveUpdates);

function subscribeToView() {
  browserNotifications.updateViewing();
  if (document.hidden) {
    liveUpdates.use(browserNotificationsEnabled() ? '/api/events' : null);
    return;
  }
  const params = new URLSearchParams();
  if (view) params.set('bot', view.id);
  liveUpdates.use(params.size ? `/api/events?${params}` : '/api/events');
}
function refreshSessionDate() {
  clearTimeout(sessionDateRefresh);
  if (document.hidden) return;
  const now = new Date();
  const midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1);
  assignSessionDateRefresh(setTimeout(refreshSessionDate, midnight - now));
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
onNotificationPreferenceChange(resumeUpdates);
document.addEventListener('visibilitychange', () => {
  assignVisibilityGeneration(visibilityGeneration + 1);
  resumeUpdates();
});
window.addEventListener('pagehide', () => {
  assignVisibilityGeneration(visibilityGeneration + 1);
  clearTimeout(sessionDateRefresh);
  liveUpdates.use(null);
});
window.addEventListener('pageshow', (event) => { if (event.persisted) resumeUpdates(); });
window.addEventListener('popstate', () => { if (listLoaded) selectLocation(); });
