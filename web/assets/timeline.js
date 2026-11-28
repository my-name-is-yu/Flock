import { taskWaitDecision } from './board-inbox.js';
import { orderSetupNotices, setupNotice } from './bot-setup.js';
import { isMemoryNotice, isSystemNotice, messagePlacement, messageSide } from './chat-grouping.mjs';
import { closedLabels, pendingActionLabels, stateLabels } from './conversation-copy.mjs';
import { node } from './dom.js';
import { timelineMessages } from './event-history.mjs';
import { messageAttachments } from './message-attachments.js';
import { hasQuestionChoices, questionChoiceForm, questionReplyButton } from './question-choices.js';
import { replyArrivalId } from './reply-arrival.mjs';
import { createTimelineRows } from './timeline-rows.js';
import {
  assignTimelineHeight,
  assignTimelineWidth,
  botQuery,
  bots,
  byId,
  closeDetail,
  composition,
  dayFormat,
  decisions,
  elements,
  findPanel,
  fullTime,
  listLoaded,
  motionAnimations,
  motionEaseOut,
  noHover,
  openDetail,
  openNewBot,
  operate,
  reducedMotion,
  refreshBots,
  render,
  renderComposer,
  screenshotsFor,
  sendingMessages,
  setNotice,
  sidebarViewer,
  timelineHeight,
  view,
} from './app-state.js';
import {
  animateLiveReply,
  animateSentMessage,
  animateTimelineContent,
  api,
  avatar,
  button,
  cancelMotion,
  changed,
  linkToRun,
  quotedText,
  runMotion,
  waitText,
} from './app-core.js';

const updateTimelineRows = createTimelineRows(elements.timeline, elements.conversation);
const clock = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
const instructionTime = new Intl.DateTimeFormat('en', { dateStyle: 'medium', timeStyle: 'medium', hourCycle: 'h23' });
const shortDay = new Intl.DateTimeFormat('en', { month: 'short', day: 'numeric' });
const readingPositions = new Map();
const timelineKey = (botId) => JSON.stringify([botId]);

const participant = (id) => bots.find((bot) => bot.id === id) ?? { id, name: id };

export function beginReply(target, text = '') {
  const composer = view?.composer;
  if (!target.messageId || !composer || sendingMessages.has(composer)) return;
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
function setMessageHtml(element, html) {
  element.innerHTML = html;
  for (const link of element.querySelectorAll('a[href]')) {
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
  }
}
function iconButton(name, label, click) {
  const control = button(null, 'icon-button msg-action', click);
  control.dataset.timelineAction = name;
  control.setAttribute('aria-label', label);
  control.dataset.tooltip = label;
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
    control.dataset.tooltip = 'Copied';
    control._copyTimer = setTimeout(() => {
      delete control.dataset.copied;
      control.replaceChildren(icon('copy'));
      control.setAttribute('aria-label', 'Copy text');
      control.dataset.tooltip = 'Copy text';
    }, 1600);
  } catch { if (view) setNotice(view, 'Couldn’t copy the text.', true); }
}
export function questionActions(context, task, { dialog = false } = {}) {
  const target = { kind: 'wait', messageId: task.wait.messageId, summary: waitText(task.wait) };
  const actions = node('div', dialog ? 'wait-primary' : 'event-actions question-actions');
  const choices = task.wait.choices;
  const item = taskWaitDecision(context.id, task);
  const busy = decisions.busy(item);
  if (hasQuestionChoices(choices)) {
    const { key, text, stale } = decisions.choice(item);
    actions.append(questionChoiceForm({
      key, prompt: target.summary, choices, dialog, answer: text, busy: busy || stale,
      select: (answer) => decisions.selectChoice(item, answer),
      submit: (answer) => void operate(context, task, 'answer', { text: answer }),
    }));
  }
  const reply = questionReplyButton(() => {
    if (view?.id === context.id) beginReply(target);
  }, dialog);
  reply.disabled = busy;
  actions.append(reply);
  return actions;
}
export function readOnlyChoices(choices) {
  const options = node('ul', 'read-only-choices');
  for (const choice of choices) options.append(node('li', null, choice));
  return options;
}
export function nextSuggestion(context) {
  const composer = context?.composer;
  if (context?.data?.canDecide !== true || context.source || !composer) return null;
  const composerBusy = sendingMessages.has(composer) || composition || decisions.composer() || composer.reply;
  if (composerBusy || composer.textAt(0)) return null;
  if (['working', 'thinking'].includes(context.data.presence)) return null;
  let latest = null;
  for (const event of context.events.values()) if (event.kind !== 'wait_closed' && (!latest || event.id > latest.id)) latest = event;
  if (latest?.kind !== 'conversation_reply') return null;
  const text = latest.data.suggestions?.[0];
  if (typeof text !== 'string' || !text.trim()) return null;
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
function eventTime(event, extra) {
  const timestamp = new Date(event.time * 1000);
  const time = node('time', 'msg-time', clock.format(timestamp));
  time.dateTime = timestamp.toISOString();
  time.setAttribute('aria-description', [fullTime.format(timestamp), extra].filter(Boolean).join(' · '));
  return time;
}

function chatTimeBreak(event) {
  const separator = node('p', 'day chat-time-break');
  separator.setAttribute('aria-hidden', 'true');
  separator.append(eventTime(event));
  return separator;
}

export function messageBubble(target) {
  if (!(target instanceof Element)) return null;
  const bubble = target.closest('.event-message .bubble');
  const control = target.closest('a, button, input, select, textarea, label, summary, [controls], '
    + '[contenteditable]:not([contenteditable="false"]), [tabindex], [role="button"], [role="link"]');
  return bubble?.contains(control) ? null : bubble;
}

export function closeMessageTime() {
  const context = view;
  if (!context?.revealedTimeRow) return;
  const row = byId(context.revealedTimeRow)?.closest('.event-message');
  const time = row?.querySelector(':scope > .msg-time');
  if (time) row.querySelector('.msg-actions').prepend(time);
  context.revealedTimeRow = null;
}

function conversationMessageRow(id) {
  // A collapsed Session report can be an anchor inside the completion row.
  return byId(`event-${id}`)?.closest('article');
}
export function resetConversationJump(context) {
  if (context) context.pendingSource = null;
  elements['conversation-source-error'].textContent = '';
}
export function leaveConversationSource(context) {
  resetConversationJump(context);
  if (!context.source) return false;
  context.source = null;
  readingPositions.delete(timelineKey(context.id));
  return true;
}
export function backToLatest() {
  leaveConversationSource(view);
  render('bottom');
  elements.conversation.scrollTop = elements.conversation.scrollHeight;
  elements.conversation.focus({ preventScroll: true });
}
export function updateFindOverlap() {
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
export async function showConversationMessage(context, id, { bot, signal, preferLoaded } = {}) {
  if (context !== view || signal?.aborted) return;
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
      error.textContent = 'Couldn’t load that message.';
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
  unfoldMessageRow(target);
  elements.timeline.querySelector('.conversation-source-target')?.classList.remove('conversation-source-target');
  void target.offsetWidth;
  target.classList.add('conversation-source-target');
  focusConversationMessage(target);
  updateJumpToLatest();
}
export function cardSourceAction(item, close) {
  const id = item.conversationEventId;
  if (!Number.isSafeInteger(id) || id <= 0 || id >= Number.MAX_SAFE_INTEGER) return null;
  return button('Show in conversation', 'text-button board-inbox-source', () => {
    close();
    void showConversationMessage(view, id, { bot: participant(item.conversationBotId ?? item.botId), preferLoaded: true });
  });
}
// Rendered Markdown is block content, so an icon that belongs beside the words goes inside its first or last paragraph.
function inlineEdge(text, last) {
  const block = last ? text.lastElementChild : text.firstElementChild;
  return text.classList.contains('markdown-body') && block?.tagName === 'P' ? block : text;
}
function setBody(text, html, plain) {
  if (typeof html === 'string') {
    text.classList.add('markdown-body');
    setMessageHtml(text, html);
  } else text.append(document.createTextNode(plain ?? ''));
}
function appendTrailingIcon(body, icon) {
  const text = inlineEdge(body, true);
  const last = text.lastChild;
  const at = last?.nodeType === Node.TEXT_NODE ? last.data.trimEnd().search(/\S+$/) : -1;
  // Long URLs and unspaced CJK text still need to wrap inside the bubble.
  if (at >= 0 && last.data.trimEnd().length - at <= 24) {
    const word = last.splitText(at);
    const glued = node('span', 'trailing-icon');
    glued.append(word, icon);
    text.append(glued);
  } else text.append(icon);
}
function runIcon(run) {
  const link = linkToRun(run);
  if (!link) return null;
  link.className = 'run-link';
  link.dataset.timelineAction = `run:${link.href}`;
  link.dataset.tooltip = 'Open in T3';
  link.setAttribute('aria-label', 'Open in T3');
  link.replaceChildren(icon('open'));
  return link;
}
function verificationLine(run, showLink) {
  const status = Object.hasOwn(stateLabels, run.state) ? stateLabels[run.state] : run.state;
  const line = node('p', 'event-reason', [run.name, status].filter(Boolean).join(' · '));
  if (run.commit) line.append(' · ', node('code', null, run.commit));
  if (showLink) {
    const link = runIcon(run);
    if (link) {
      link.setAttribute('aria-label', `Open ${run.name} in T3`);
      line.append(link);
    }
  }
  return line;
}

function memoryUndo(event, context, status) {
  const { data } = event;
  const undo = button('Undo', 'text-button', async () => {
    if (undo.getAttribute('aria-disabled') === 'true') return;
    undo.setAttribute('aria-disabled', 'true');
    status.textContent = 'Undoing…';
    try {
      await api('memory/revert', { botId: context.id, docId: data.docId, revisionId: data.revisionId, baseVersion: data.version });
    } catch (error) {
      if (error.status === 409) {
        status.textContent = 'Memory has changed.';
        if (context === view) void context.refresh();
      } else {
        status.textContent = 'Couldn’t undo. Try again.';
        undo.removeAttribute('aria-disabled');
      }
      return;
    }
    const current = context.events.get(event.id);
    if (current) context.events.set(event.id, { ...current, data: { ...current.data, canUndo: false, undone: true } });
    if (context === view) {
      renderTimeline();
      void context.refresh();
    }
  });
  undo.dataset.timelineAction = 'undo-memory';
  return undo;
}

function memoryNotice(event, canUndo, context) {
  const { data } = event;
  const learned = event.kind === 'memory_learned';
  const note = node('article', 'system-note memory-notice');
  note.id = `event-${event.id}`;
  note.tabIndex = -1;
  const kind = learned ? 'Learned' : 'Remembered';
  note.append(node('strong', null, data.undone ? 'Memory undone' : kind), ' · ',
    learned ? data.added.map((fact) => `“${fact}”`).join(' · ') : data.text);
  if (learned) {
    const source = button(data.taskTitle || 'Source request', 'text-button', () => {
      void showConversationMessage(context, data.requestEventId, { preferLoaded: true });
    });
    source.dataset.timelineAction = 'show-learning-source';
    note.append(' · from ', source);
  }
  const stale = !!data.revisionId && !data.canUndo && !data.undone;
  const status = node('span', 'memory-notice-status', stale ? 'Memory has changed.' : '');
  status.setAttribute('role', 'status');
  if (canUndo) note.append(' · ', memoryUndo(event, context, status));
  if (learned && data.removed.length) {
    note.append(node('p', 'memory-notice-removed', 'Replaced · ' + data.removed.map((fact) => `“${fact}”`).join(' · ')));
  }
  if (canUndo || stale) note.append(status);
  return note;
}

function eventCard(event, context, place, report) {
  const data = event.data;
  const bot = context.bot;
  const { side } = place;
  if (event.kind === 'bot_setup_updated')
    return setupNotice(event, !context.readOnly && bot?.canDelete, async () => {
      const active = view;
      await api('bots/setup/undo', { botId: bot.id, eventId: event.id });
      const current = active.events.get(event.id);
      if (current) active.events.set(event.id, { ...current, data: { ...current.data, undone: true } });
      if (active === view) {
        renderTimeline();
        void active.refresh();
        void refreshBots();
      }
    });
  if (isMemoryNotice(event))
    return memoryNotice(event, !context.readOnly && bot?.canDelete && data.canUndo, view);
  if (event.kind === 'answer_delivery_unconfirmed') {
    const note = node('article', 'system-note session-event', 'T3 did not confirm receipt of the answer · ');
    note.id = `event-${event.id}`;
    note.tabIndex = -1;
    note.append(eventTime(event));
    return note;
  }
  if (data.botMessage) {
    const note = node('article', 'system-note session-event', `From ${data.botMessage.fromName} · `);
    note.id = `event-${event.id}`;
    note.tabIndex = -1;
    note.append(eventTime(event));
    note.append(node('p', 'session-invitation-note', data.text ?? ''));
    return note;
  }
  if (data.sessionWake || data.sessionInvitation) {
    const session = data.sessionWake ?? data.sessionInvitation;
    const name = `T3 thread “${session.title || session.threadId}”`;
    const wakeText = {
      finished: `Work finished in ${name}`,
      failed: `Work stopped before finishing in ${name}`,
      interrupted: `Work was interrupted in ${name}`,
      waiting_for_person: `Work in ${name} needs your input`,
      unknown: `Flock couldn’t check the status of work in ${name}`,
      check_due: `Time to check on work in ${name}`,
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
  const text = node('div', 'event-text');
  const actions = node('div', 'msg-actions');
  actions.append(time);
  const extras = [];
  let quote = null;
  let replyTarget = null;
  let copy = data.text ?? '';
  const pendingWait = event.kind === 'waiting' && !context.readOnly ? context.waits.get(data.id) : null;

  if (event.kind === 'waiting') {
    copy = waitText(data, { spokenByBot: true });
    text.textContent = copy;
    bubble.append(text);
    const choices = pendingWait?.wait.choices ?? data.choices;
    if (!context.canDecide && choices?.length) bubble.append(readOnlyChoices(choices));
    if (context.canDecide && pendingWait?.wait.reason === 'user_question') extras.push(questionActions(view, pendingWait));
    else if (pendingWait) {
      if (pendingWait.wait.reason !== 'policy_approval') {
        const open = node('div', 'event-actions question-actions');
        const label = pendingActionLabels[pendingWait.wait.reason] ?? 'View request…';
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
    setBody(text, data.textHtml, data.text);
    text.classList.add('foldable-text');
    bubble.append(text);
    if (data.suggestions?.length) {
      if (context.canDecide) extras.push(suggestionActions(event));
      else bubble.append(readOnlyChoices(data.suggestions));
    }
    replyTarget = ['Reply to this message', { kind: 'message', messageId: event.id, summary: data.text }];
  } else if (event.kind === 'report') {
    copy = data.evidenceStatus === 'report_unavailable' ? 'The work ended, but its report couldn’t be loaded.'
      : data.report || (data.destination ? 'No report is available.' : 'The work is done.');
    setBody(text, copy === data.report ? data.textHtml : undefined, copy);
    text.classList.add('foldable-text');
    const link = runIcon(data.run);
    if (link) appendTrailingIcon(text, link);
    bubble.append(text);
    if (data.report) replyTarget = ['Reply to this result', { kind: 'result', messageId: event.id, summary: data.report }];
    if (data.destination) {
      bubble.append(verificationLine({name: data.destination, state: data.status, commit: data.commit}, false));
    } else if (typeof data.status === 'string' && data.status && data.status !== 'completed') {
      const status = Object.hasOwn(stateLabels, data.status) ? stateLabels[data.status] : data.status;
      bubble.append(node('p', 'event-reason', `Work ${status.toLowerCase()}`));
    }
    if ((data.evidenceStatus || data.destination) && data.detail) bubble.append(node('p', 'event-reason', data.detail));
    if (data.truncated) bubble.append(node('p', 'event-reason', 'Open T3 to read the full report.'));
  } else if (event.kind === 'completed') {
    const outcome = data.runStatus;
    setBody(text, data.textHtml, data.text);
    const lead = inlineEdge(text, false);
    if (outcome === 'completed') {
      lead.prepend(icon('check', 'glyph kind-icon result-success'), node('span', 'sr-only', 'Work complete: '));
    } else if (['failed', 'cancelled', 'interrupted', 'rolled_back'].includes(outcome)) {
      const tone = outcome === 'failed' ? 'result-failed' : 'result-interrupted';
      lead.prepend(icon('resultWarning', `glyph kind-icon ${tone}`), node('span', 'sr-only', `Work ${stateLabels[outcome].toLowerCase()}: `));
    }
    const link = runIcon(data.run);
    if (link) appendTrailingIcon(text, link);
    bubble.append(text);
    if (data.text) replyTarget = ['Reply to this result', { kind: 'result', messageId: event.id, summary: data.text }];
  } else {
    setBody(text, data.textHtml, data.text);
    if (trigger) inlineEdge(text, false).prepend(icon('clock', 'glyph kind-icon'), node('span', 'sr-only', 'Automated request: '));
    bubble.append(text);
    text.classList.add('foldable-text');
    if (event.kind === 'conversation_message') {
      const count = data.attachmentCount;
      const noun = count === data.imageAttachmentCount ? 'image' : 'file';
      if (data.files?.length) bubble.append(messageAttachments(bot.id, data.messageId, data.files));
      else if (count > 0) bubble.append(node('p', 'message-attachments', `${count} ${noun}${count === 1 ? '' : 's'} attached`));
    }
    if (event.kind === 'user_answer') {
      const question = data.question;
      if (question && !place.afterQuestion) {
        quote = node('p', 'msg-quote');
        quote.append(icon('reply'), node('span', null, question));
        quote.dataset.tooltip = question;
      }
    }
    if (event.kind === 'conversation_message' && place.replyQuote) {
      quote = button(null, 'msg-quote msg-quote-link', () => {
        void showConversationMessage(view, data.replyToMessageId, { preferLoaded: true });
      });
      quote.dataset.timelineAction = 'open-original';
      quote.setAttribute('aria-label', `Show the message this replies to: ${place.replyQuote}`);
      quote.append(icon('reply'), node('span', null, place.replyQuote));
      quote.dataset.tooltip = place.replyQuote;
    }
    if (data.channel?.channelKind) extras.push(node('p', 'msg-note channel-source', channelLabel(data.channel.channelKind)));
    else if (event.id === context.latestWebMessageId && data.status === 'received')
      extras.push(node('p', 'msg-note', 'Received'));
    if (data.instructionAcceptedAt != null)
      extras.push(node('p', 'msg-note', `Instruction accepted · ${instructionTime.format(data.instructionAcceptedAt * 1000)}`));
    if (data.instructionDeliveredAt != null)
      extras.push(node('p', 'msg-note', `Delivery to the next T3 Run confirmed · ${instructionTime.format(data.instructionDeliveredAt * 1000)}`));
    if (['request', 'conversation_message'].includes(event.kind)) {
      for (const run of context.parallelTasks.get(event.taskId) ?? []) {
        bubble.append(verificationLine(run, true));
        if (run.detail) bubble.append(node('p', 'event-reason', run.detail));
      }
    }
    const blocker = ['request', 'conversation_message'].includes(event.kind) ? context.queued.get(event.taskId)?.blockedBy : null;
    if (blocker) {
      const note = node('p', 'msg-note msg-queued');
      if (blocker.request) {
        note.append(icon('clock'), node('span', null, 'Queued after'), node('span', 'msg-queued-request', `“${blocker.request}”`));
        note.setAttribute('aria-description', `Starts when “${blocker.request}” finishes`);
      } else {
        note.append(icon('clock'), node('span', null, 'Waiting for another Bot to finish in this project'));
      }
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

  if (context.canDecide && replyTarget) {
    const [label, target] = replyTarget;
    actions.append(iconButton('reply', label, () => beginReply(target)));
  }
  if (!actions.querySelector('button:not(:disabled)')) row.tabIndex = 0;
  line.append(bubble, actions);
  row.append(...(quote ? [quote] : []), line, ...extras);
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
  ? data.activeTasks?.find(task => task.state === 'running')?.request ?? stateLabels.working : stateLabels.thinking;
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
  next.dataset.tooltip = label;
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
export function startedLabel(timestamp) {
  const day = dayLabel(timestamp / 1000);
  return day === 'Today' ? clock.format(timestamp) : day === 'Yesterday' ? day : shortDay.format(timestamp);
}
function dayLabel(seconds) {
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const days = Math.round((today - new Date(seconds * 1000).setHours(0, 0, 0, 0)) / 86400000);
  return days === 0 ? 'Today' : days === 1 ? 'Yesterday' : dayFormat.format(seconds * 1000);
}

export function timelineScrollPosition() {
  const scroller = elements.conversation;
  const height = scroller.scrollHeight;
  const top = scroller.scrollTop;
  return { height, top, atBottom: height - top - scroller.clientHeight < 65 };
}

export function timelineAtEnd(scroller, viewportHeight = scroller.clientHeight) {
  return scroller.scrollHeight - scroller.scrollTop - viewportHeight <= 4;
}

export function rememberReadingPosition(keepAtBottom = false) {
  if (view?.source) return;
  const context = view;
  if (context?.newestEvent == null || context.anchor || !context.data) return;
  const { top, atBottom } = timelineScrollPosition();
  if (atBottom && !keepAtBottom) return;
  const viewportTop = elements.conversation.getBoundingClientRect().top;
  const anchors = elements.timeline.querySelectorAll(':scope > [id]');
  const anchor = [...anchors].find((row) => row.getBoundingClientRect().bottom > viewportTop);
  const { events, olderBefore, newestEvent } = context;
  readingPositions.set(timelineKey(context.id), {
    // Retain loaded pages so an older message is available when its conversation reopens.
    history: { events, olderBefore, newestEvent, historyCursor: context.data?.historyCursor },
    top, atBottom,
    anchorId: anchor?.id,
    offset: anchor ? anchor.getBoundingClientRect().top - viewportTop : 0,
  });
}

function restoreReadingPosition(position) {
  const scroller = elements.conversation;
  if (position.atBottom) {
    scroller.scrollTop = scroller.scrollHeight;
    return;
  }
  const row = position.anchorId ? byId(position.anchorId) : null;
  const anchor = row?.closest('article') ?? row;
  scroller.scrollTop = anchor ? scroller.scrollTop + anchor.getBoundingClientRect().top -
    scroller.getBoundingClientRect().top - position.offset : position.top;
}

export function eventHistoryForRefresh(context) {
  // Reopen saved pages only after a successful read of the current conversation.
  if (context.newestEvent != null) return context;
  const cached = readingPositions.get(timelineKey(context.id));
  return cached ? { ...cached.history } : context;
}

export function updateJumpToLatest() {
  const scroller = elements.conversation;
  const frame = scroller.parentElement;
  const { height, top, atBottom } = timelineScrollPosition();
  frame.toggleAttribute('data-history-above', top > 1);
  frame.toggleAttribute('data-history-below', height - top - scroller.clientHeight > 1);
  const control = elements['jump-to-latest'];
  // An explicitly opened history page is separate from the latest messages.
  const hidden = !view?.source && atBottom;
  if (control.hidden === hidden) return;
  if (hidden && document.activeElement === control) scroller.focus({ preventScroll: true });
  control.hidden = hidden;
}

export function renderTimeline(scroll = 'auto', previousScroll = timelineScrollPosition(), liveAfterId) {
  const older = elements.older;
  const source = view?.source;
  const data = source ? null : view?.data;
  const context = view;
  const events = source?.events ?? [...(context?.events.values() ?? [])].sort((a, b) => a.id - b.id);
  elements.conversation.classList.toggle('viewing-source', !!source);
  older.hidden = !!source || !context?.olderBefore;
  older.disabled = !!context?.loadingOlder;
  const olderLabel = context?.loadingOlder ? 'Loading…' : 'Show earlier messages';
  if (older.textContent !== olderLabel) older.textContent = olderLabel;
  const scroller = elements.conversation;
  const key = context?.newestEvent != null && !context.anchor && timelineKey(context.id);
  const position = !source && key && readingPositions.get(key);
  const moveScroll = () => {
    if (scroll === 'anchor') focusConversationMessage(byId(`event-${context.anchorId}`));
    else if (scroll === 'older') scroller.scrollTop = previousScroll.top + scroller.scrollHeight - previousScroll.height;
    else if (position) {
      restoreReadingPosition(position);
      // The restored offset already includes the returning composer's height.
      assignTimelineHeight(scroller.clientHeight);
      assignTimelineWidth(scroller.clientWidth);
      scroller.classList.toggle('follow-newest', timelineAtEnd(scroller, timelineHeight));
      readingPositions.delete(key);
    } else if (scroll === 'bottom' || (!context?.anchor && previousScroll.atBottom)) scroller.scrollTop = scroller.scrollHeight;
    else scroller.scrollTop = previousScroll.top;
    updateJumpToLatest();
  };
  if (changed('timeline', view?.id, source ?? [sidebarViewer, data?.composerPerson, data?.bot, events, data?.waits, data?.queued,
    decisions.pendingKeys(), listLoaded, data?.presence, data?.activeTasks, data?.parallelTasks, data?.sessionTasks,
    bots, dayFormat.format(Date.now()),
    decisions.itemsFor(view?.id).filter(item => item.itemKind === 'policy_approval')])) {
    scroller.setAttribute('aria-label', 'Conversation history');
    const added = updateTimelineRows(context, (keep) => conversationRows(data, events, keep, source));
    syncMessageFolding(added);
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
function setBubbleFold(bubble, toggle, folded) {
  bubble.dataset.fold = folded ? 'folded' : 'unfolded';
  toggle.textContent = folded ? 'Show more' : 'Show less';
  toggle.setAttribute('aria-expanded', String(!folded));
}
function unfoldMessageRow(row) {
  const bubble = row?.querySelector('.bubble[data-fold="folded"]');
  if (!bubble) return;
  const text = bubble.querySelector('.foldable-text');
  const toggle = text?.nextElementSibling?.classList.contains('fold-toggle') ? text.nextElementSibling : null;
  if (text) {
    cancelMotion(text);
    text.style.overflow = '';
  }
  if (toggle) setBubbleFold(bubble, toggle, false);
  else bubble.dataset.fold = 'unfolded';
  if (row.id) unfoldedMessages.add(row.id);
}
export function syncMessageFolding(rows) {
  const entries = rows.flatMap((row) => [...row.querySelectorAll('.foldable-text')].map((text) => ({
    text,
    bubble: text.closest('.bubble'),
    id: text.closest('.event-message')?.id,
    existing: text.nextElementSibling?.classList.contains('fold-toggle') ? text.nextElementSibling : null,
  })));
  // Inserting or removing a toggle invalidates layout. Measure all candidates before changing any row.
  const shouldFold = entries.map(({ text }) => {
    const lineHeight = parseFloat(getComputedStyle(text).lineHeight);
    return Number.isFinite(lineHeight) && text.scrollHeight > lineHeight * (foldLines + 2);
  });
  entries.forEach(({ text, bubble, id, existing }, index) => {
    if (!bubble) return;
    if (!shouldFold[index]) {
      if (existing) existing.remove();
      delete bubble.dataset.fold;
      cancelMotion(text);
      text.style.overflow = '';
      return;
    }
    if (existing) {
      setBubbleFold(bubble, existing, !unfoldedMessages.has(id));
      return;
    }
    const toggle = button(null, 'fold-toggle', () => {
      const nextFolded = bubble.dataset.fold !== 'folded';
      const startHeight = text.getBoundingClientRect().height;
      cancelMotion(text);
      setBubbleFold(bubble, toggle, nextFolded);
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
    setBubbleFold(bubble, toggle, !unfoldedMessages.has(id));
  });
}

function conversationRows(data, events, keep, source) {
  const timeline = {
    bot: source?.bot ?? data?.bot ?? bots.find((item) => item.id === view?.id),
    // Source lookups do not establish the latest message in the live conversation.
    latestWebMessageId: data && !source ? events.findLast((event) =>
      event.kind === 'conversation_message' && !event.data.channel?.channelKind && messageSide(event) === 'user')?.id : undefined,
    // An earlier page opened from the inbox is for reading; answering happens at the latest messages.
    canDecide: !source && !!view?.composer,
    readOnly: !!source,
    revealedTimeRow: view?.revealedTimeRow,
    waits: new Map((data?.waits ?? []).map((task) => [task.wait.id, task])),
    queued: new Map((data?.queued ?? []).map((task) => [task.id, task])),
    parallelTasks: new Map((data?.parallelTasks ?? []).map((task) => [task.id, task.runs])),
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
  const shown = timelineMessages(orderSetupNotices(events), dayLabel);
  const eventsById = new Map(events.map((event) => [event.id, event]));
  const rows = [];
  let day = '';
  let previous = null;
  let previousReport = null;
  shown.forEach(({ event, report }, index) => {
    const eventDay = dayLabel(event.time);
    if (eventDay !== day) {
      rows.push(keep(`day-${event.id}`, eventDay, () => node('p', 'day', eventDay)));
      day = eventDay;
      previous = previousReport = null;
    }
    const place = messagePlacement(previous, event, shown[index + 1]?.event);
    const replyId = event.kind === 'conversation_message' ? event.data.replyToMessageId : 0;
    place.replyQuote = replyId && ![previous?.id, previousReport?.id].includes(replyId) ? quotedText(eventsById.get(replyId)) : null;
    if (place.timeBreak) rows.push(keep(`gap-${event.id}`, event.time, () => chatTimeBreak(event)));
    const approval = replacements.get(event.id);
    if (approval) {
      rows.push(keep(`event-${event.id}`, ['approval', approval, decisions.busy(approval)], () => {
        const card = decisions.inlinePolicyCard(approval);
        card.id = `event-${event.id}`;
        card.tabIndex = -1;
        return card;
      }));
      previous = event;
      previousReport = null;
      return;
    }
    const wait = event.kind === 'waiting' ? timeline.waits.get(event.data.id) : null;
    const waitDecision = wait && taskWaitDecision(view.id, wait);
    const request = ['request', 'conversation_message'].includes(event.kind);
    const blocker = request ? timeline.queued.get(event.taskId)?.blockedBy : null;
    const row = keep(`event-${report?.id ?? event.id}`,
      [event, report, botVersion, timeline.canDecide, timeline.readOnly, event.id === timeline.latestWebMessageId,
        place.groupStart, place.taskStart, place.afterQuestion, place.replyQuote, wait,
        event.kind === 'waiting' ? timeline.closed.get(event.data.id) : null,
        waitDecision && decisions.busy(waitDecision),
        wait?.wait.reason === 'user_question' && decisions.choice(waitDecision), blocker,
        request ? timeline.parallelTasks.get(event.taskId) : null,
        isSystemNotice(event) && bot?.canDelete,
        event.kind === 'waiting' && !!view.composer?.textAt(event.id)],
      () => eventCard(event, timeline, place, report));
    if (place.side === 'bot') syncTimelineAvatar(row, place.groupEnd && bot, bot);
    row.classList.toggle('group-continues', !place.groupEnd);
    row.classList.toggle('conversation-source-target', event.id === source?.id);
    rows.push(row);
    for (const item of approvals) {
      if (!anchoredToWait.has(item.itemId) && item.conversationEventId === event.id)
        rows.push(keep(`policy-${item.itemId}`, [item, decisions.busy(item)], () => decisions.inlinePolicyCard(item)));
    }
    previous = event;
    previousReport = report;
  });
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
          node('p', null, 'A Bot takes your requests, gets the work done and reports back here.'),
          button('New Bot', 'button primary', () => openNewBot(elements['new-bot-button'])));
      } else {
        empty.append(node('h2', null, loaded ? 'No Bots' : 'Loading…'));
      }
      return empty;
    }));
  }
  if (data && !data.bot.paused && (data.presence === 'working' || data.presence === 'thinking')) {
    const row = keep('working', botVersion, () => workingLine(bot));
    setWorkingText(row, workingText(data));
    rows.push(row);
  }
  return rows;
}
