import { hasQuestionChoices, questionChoiceForm, questionReplyButton, updateQuestionChoiceForm } from './question-choices.js';
import { actionLabel, approvalSummary } from './action-labels.mjs';
import { node } from './dom.js';

// One name per pending item, said as what the person needs to do; the sidebar and this panel share it.
export const pendingKindLabels = {
  user_question: 'Question for you',
  protected_input: 'Secret needed',
  policy_approval: 'Approval needed',
};
const labels = {
  ...pendingKindLabels,
  answer: 'Answered', cancel: 'Stop requested', deny: 'Denied', supply: 'Secret saved',
  allow_once: 'Allowed once', always_allow: 'Allowed for this Flock',
  allow_next_time: 'Allow rule saved for this Flock',
};
const historyKinds = { user_question: 'Question', protected_input: 'Secret request', policy_approval: 'Approval' };
const actions = {
  protected_input: [['supply', 'Save secret']],
  policy_approval: [['allow_once', 'Allow once'], ['always_allow', 'Allow for this Flock'], ['deny', 'Deny']],
};
const identity = ['botId', 'itemKind', 'itemId', 'waitId', 'taskId', 'targetRevision', 'targetGeneration'];
const draftStorage = 'flock-inbox-answer-drafts';

const itemKey = (item) => JSON.stringify([item.botId, item.itemKind, item.itemId]);
const decisionTarget = (item) => JSON.stringify([...identity.map((field) => item[field]), item.request, item.prompt, item.choices, item.status]);
// Task waits use the wait ID in both views. Policy approvals have their own identity and generation.
const isTaskWait = (item) => item.taskId > 0 && item.waitId > 0 &&
  (!item.itemKind || (item.itemId === item.waitId && item.itemKind !== 'policy_approval'));
const waitKey = (item) => JSON.stringify([item.botId, item.taskId, item.waitId]);
const choiceTarget = (item) => decisionTarget({ ...item, itemKind: 'user_question', itemId: item.waitId });
const savesFlockRule = (decision) => decision === 'always_allow' || decision === 'allow_next_time';

export function taskWaitDecision(botId, task) {
  const { wait } = task;
  return { botId, taskId: task.id, waitId: wait.id, targetRevision: task.currentRevision ?? 0,
    targetGeneration: task.generation, request: task.request, prompt: wait.text, choices: wait.choices ?? [] };
}

function labeled(text, control) {
  const label = node('label', null, text);
  label.append(control);
  return label;
}
const clockFormat = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
const dayFormat = new Intl.DateTimeFormat('en', { month: 'short', day: 'numeric' });
const olderDate = new Intl.DateTimeFormat('en', { dateStyle: 'medium' });
// "Today, 19:10", "Yesterday, 08:02" or "Nov 20, 14:30", matching the conversation's clock.
function time(seconds) {
  const date = new Date(seconds * 1000);
  const days = Math.round((new Date().setHours(0, 0, 0, 0) - new Date(date).setHours(0, 0, 0, 0)) / 86400000);
  const day = days === 0 ? 'Today' : days === 1 ? 'Yesterday' : dayFormat.format(date);
  return `${day}, ${clockFormat.format(date)}`;
}
function relativeTime(seconds) {
  if (!Number.isFinite(seconds) || seconds <= 0) return '';
  const delta = Math.max(0, Math.floor(Date.now() / 1000 - seconds));
  if (delta < 60) return 'Just now';
  if (delta < 3600) {
    const mins = Math.floor(delta / 60);
    return `${mins} ${mins === 1 ? 'min' : 'mins'} ago`;
  }
  if (delta < 86400) {
    const hours = Math.floor(delta / 3600);
    return `${hours} ${hours === 1 ? 'hour' : 'hours'} ago`;
  }
  const days = Math.floor(delta / 86400);
  if (days < 30) return `${days} ${days === 1 ? 'day' : 'days'} ago`;
  return olderDate.format(seconds * 1000);
}
function historyCard(entry) {
  const article = node('article', 'board-inbox-card history-card');
  const approval = entry.itemKind === 'policy_approval' && entry.action;
  const title = approval ? actionLabel(entry.action) : entry.requestTitle || historyKinds[entry.itemKind] || 'Request';
  const heading = node('h3', null, title);
  if (approval && entry.target) heading.append(node('span', 'board-inbox-target', entry.target));
  article.append(heading);
  // Name the kind of item only when the title doesn't already say it, then who answered and when.
  const about = approval ? entry.requestTitle : entry.requestTitle && historyKinds[entry.itemKind];
  if (about) article.append(node('p', 'history-about', about));
  const by = entry.decidedByYou ? 'you' : entry.decidedBy;
  article.append(node('p', 'board-inbox-meta', `${labels[entry.decision] ?? 'Answered'} by ${by} · ${time(entry.decidedAt)}`));
  return article;
}
function button(text, className, click) {
  const control = node('button', className, text);
  control.type = 'button';
  control.addEventListener('click', click);
  return control;
}
function setCardBusy(card, busy) {
  for (const control of card.querySelector('.board-inbox-form')?.elements ?? []) control.disabled = busy;
  for (const control of card.querySelectorAll('.board-inbox-stop button')) control.disabled = busy;
}

function stopSection(item, submit, busy) {
  const container = node('div', 'board-inbox-stop');
  const showDefault = (restoreFocus = false) => {
    const stop = button('Stop this request', 'wait-cancel', showConfirm);
    stop.disabled = busy;
    container.replaceChildren(stop);
    if (restoreFocus) stop.focus();
  };
  function showConfirm() {
    const confirm = node('div', 'wait-stop-confirm');
    confirm.setAttribute('role', 'group');
    const prompt = node('p', 'wait-stop-text');
    prompt.id = `inbox-stop-text-${item.waitId}`;
    prompt.append(node('strong', null, 'Stop this request?'),
      ' This requests a stop for this work, including any T3 work assigned to it.');
    confirm.setAttribute('aria-labelledby', prompt.id);
    const keep = button('Keep working', 'button quiet wait-stop-keep', () => showDefault(true));
    const confirmStop = button('Stop request', 'button danger', () => void submit(item, 'cancel', {}));
    keep.disabled = busy;
    confirmStop.disabled = busy;
    const choices = node('div', 'board-inbox-actions');
    choices.append(keep, confirmStop);
    confirm.append(prompt, choices);
    container.replaceChildren(confirm);
    keep.focus();
  }
  showDefault();
  return container;
}

function decisionCard(item, submit, source, question, busy) {
  const article = node('article', 'board-inbox-card');

  const titleGroup = node('div', 'board-inbox-title-group');
  const approval = item.itemKind === 'policy_approval' && item.action;
  titleGroup.append(node('h3', null, approval ? actionLabel(item.action) : item.requestTitle));

  const submeta = node('div', 'board-inbox-submeta');
  const parts = [];
  if (item.itemKind !== 'policy_approval' && labels[item.itemKind]) {
    parts.push(node('span', null, labels[item.itemKind]));
  }
  const target = item.displayTarget ?? item.target;
  if (approval && target) parts.push(node('span', 'board-inbox-target', target));
  if (item.status === 'expired') parts.push(node('span', null, 'Expired'));
  const rel = relativeTime(item.createdAt);
  if (rel) parts.push(node('span', null, rel));
  if (source) parts.push(source);
  parts.forEach((part, idx) => {
    if (idx > 0) submeta.append(node('span', 'board-inbox-submeta-sep', '·'));
    submeta.append(part);
  });
  if (parts.length) titleGroup.append(submeta);
  article.append(titleGroup);

  const bodyLines = [];
  if (item.request && item.request !== item.requestTitle) bodyLines.push(item.request);
  const prompt = approval ? approvalSummary(item.prompt, item) : item.prompt;
  if (prompt && prompt !== item.requestTitle && prompt !== item.request) bodyLines.push(prompt);
  const text = bodyLines.join('\n').trim();
  if (text) article.append(node('p', 'board-inbox-body', text));

  const expiredApproval = item.itemKind === 'policy_approval' && item.status === 'expired';
  if (!item.canDecide || (expiredApproval && item.canSaveRule !== true)) {
    if (item.itemKind !== 'policy_approval') article.append(node('p', 'board-inbox-meta', 'View only'));
    return article;
  }
  if (item.itemKind === 'user_question') {
    article.append(question());
    if (isTaskWait(item)) article.append(stopSection(item, submit, busy));
    return article;
  }
  const form = node('form', 'board-inbox-form');
  const inputs = {};
  const buttons = node('div', 'board-inbox-actions');
  if (item.itemKind === 'protected_input') {
    inputs.value = node('input');
    inputs.value.type = 'password';
    inputs.value.autocomplete = 'off';
    inputs.value.required = true;
    form.append(labeled('Secret value', inputs.value));
  }
  const choices = expiredApproval ? [['allow_next_time', 'Allow next time for this Flock']] : actions[item.itemKind];
  for (const [decision, title] of choices) {
    if (savesFlockRule(decision) && item.canSaveRule !== true) continue;
    const btn = node('button', ['deny', 'always_allow'].includes(decision) ? 'secondary' : 'primary', title);
    btn.value = decision;
    btn.type = 'submit';
    buttons.append(btn);
  }
  form.append(buttons);
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    if (!event.submitter || event.submitter.disabled) return;
    const data = Object.fromEntries(Object.entries(inputs).map(([field, input]) => [field, input.value]));
    void submit(item, event.submitter.value, data);
  });
  article.append(form);
  if (isTaskWait(item)) article.append(stopSection(item, submit, busy));
  setCardBusy(article, busy);
  return article;
}

export function mountBoardInbox({
  api, sourceAction, onChange, onAccepted, onRefreshError, refreshConversation = () => {},
  canCompose, focusComposer, canDismiss = () => true,
}) {
  const byId = (id) => document.getElementById(id);
  const main = byId('main');
  const trigger = byId('needs-you');
  const caption = byId('profile-heading');
  const layer = byId('decision-layer');
  const shade = byId('decision-shade');
  const panel = byId('decision-panel');
  const panelBody = panel?.querySelector('.decision-panel-body');
  const title = byId('decision-heading');
  const position = byId('decision-position');
  const previous = byId('decision-previous');
  const next = byId('decision-next');
  const historyToggle = byId('decision-history');
  const message = byId('decision-message');
  const host = byId('decision-item');
  const transcript = byId('conversation');
  const reducedMotion = matchMedia?.('(prefers-reduced-motion: reduce)');
  let drafts = new Map();
  try { drafts = new Map(JSON.parse(sessionStorage.getItem(draftStorage))); } catch {}
  const saving = new Map();
  let bot, viewer, origin, mode, selected, rendered;
  let items = [];
  let epoch = 0;
  let historyRequest;
  let morphCleanup = null;

  const scoped = (botId = bot) => items.filter((item) => item.botId === botId || item.conversationBotId === botId);
  const pending = (botId) => scoped(botId).filter((item) =>
    item.itemKind !== 'policy_approval' || item.status !== 'expired' || item.canSaveRule === true);
  const current = () => mode === 'pending' ? pending().find((item) => itemKey(item) === selected) : undefined;
  const isOpen = () => !!mode;
  const count = (botId) => pending(botId).length;
  const saveKey = (item) => isTaskWait(item) ? waitKey(item) : itemKey(item);
  const inFlight = () => saving.get(viewer);
  const busy = (item) => inFlight()?.has(saveKey(item)) ?? false;
  const draftKey = (item, choice) => JSON.stringify([viewer, item.botId, item.taskId, ...(choice ? ['choice'] : [])]);

  function clearMorph() {
    if (morphCleanup) {
      morphCleanup();
      morphCleanup = null;
    }
  }

  function animateOpenFromPill(pillRect) {
    if (reducedMotion?.matches || !panel?.animate || !pillRect || !pillRect.width || !pillRect.height) return;
    const cardRect = panel.getBoundingClientRect();
    if (!cardRect.width || !cardRect.height) return;
    const dx = (pillRect.left + pillRect.width / 2) - (cardRect.left + cardRect.width / 2);
    const dy = pillRect.top - cardRect.top;
    const sx = Math.max(.18, Math.min(1, pillRect.width / cardRect.width));
    const sy = Math.max(.14, Math.min(1, pillRect.height / cardRect.height));
    panel.getAnimations().forEach((anim) => anim.cancel());
    panel.animate([
      { transform: `translate(${dx}px, ${dy}px) scale(${sx}, ${sy})`, borderRadius: '14px', opacity: .88 },
      { transform: 'translate(0, 0) scale(1, 1)', borderRadius: '20px', opacity: 1 },
    ], { duration: 240, easing: 'cubic-bezier(0.22, 1, 0.36, 1)' });
    if (panelBody?.animate) {
      panelBody.getAnimations().forEach((anim) => anim.cancel());
      panelBody.animate([
        { opacity: 0, filter: 'blur(2.5px)', transform: 'translateY(6px)' },
        { opacity: 1, filter: 'blur(0px)', transform: 'translateY(0)' },
      ], { duration: 210, easing: 'cubic-bezier(0.23, 1, 0.32, 1)', delay: 25, fill: 'backwards' });
    }
    if (shade?.animate) {
      shade.getAnimations().forEach((anim) => anim.cancel());
      shade.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 190, easing: 'ease-out' });
    }
  }

  function animateStep(direction) {
    if (reducedMotion?.matches || !host?.animate) return;
    host.getAnimations?.().forEach((anim) => anim.cancel());
    host.animate([
      { opacity: .2, transform: `translateX(${direction * 18}px)` },
      { opacity: 1, transform: 'translateX(0)' },
    ], { duration: 200, easing: 'cubic-bezier(0.23, 1, 0.32, 1)' });
  }

  function animateHistoryCards(cards) {
    if (reducedMotion?.matches) return;
    cards.forEach((card, index) => {
      card.animate?.([
        { opacity: 0, transform: 'translateY(6px)' },
        { opacity: 1, transform: 'none' },
      ], {
        duration: 200,
        easing: 'ease-out',
        delay: Math.min(index, 12) * 35,
        fill: 'backwards',
      });
    });
  }

  function animateCloseToPill(cardRect, pillRect) {
    clearMorph();
    if (reducedMotion?.matches || !main || !cardRect || !cardRect.width || !cardRect.height || !pillRect || !pillRect.width || !pillRect.height) return;
    const overlay = node('div', 'decision-morph-overlay');
    overlay.setAttribute('aria-hidden', 'true');
    const ghostShade = node('div', 'decision-morph-shade');
    const ghostCard = node('div', 'decision-morph-card');
    ghostCard.style.left = `${cardRect.left}px`;
    ghostCard.style.top = `${cardRect.top}px`;
    ghostCard.style.width = `${cardRect.width}px`;
    ghostCard.style.height = `${cardRect.height}px`;
    overlay.append(ghostShade, ghostCard);
    main.classList.add('decision-morphing');
    main.append(overlay);

    const dx = (pillRect.left + pillRect.width / 2) - (cardRect.left + cardRect.width / 2);
    const dy = pillRect.top - cardRect.top;
    const sx = Math.max(.18, Math.min(1, pillRect.width / cardRect.width));
    const sy = Math.max(.14, Math.min(1, pillRect.height / cardRect.height));

    const finish = () => {
      overlay.remove();
      main.classList.remove('decision-morphing');
      if (morphCleanup === finish) morphCleanup = null;
    };
    morphCleanup = finish;

    if (!ghostCard.animate || !ghostShade.animate) {
      finish();
      return;
    }
    const cardAnim = ghostCard.animate([
      { transform: 'translate(0, 0) scale(1, 1)', borderRadius: '20px', opacity: 1 },
      { transform: `translate(${dx}px, ${dy}px) scale(${sx}, ${sy})`, borderRadius: '14px', opacity: .2 },
    ], { duration: 195, easing: 'cubic-bezier(0.32, 0.72, 0, 1)', fill: 'forwards' });
    ghostShade.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 195, easing: 'ease-out', fill: 'forwards' });
    cardAnim.onfinish = finish;
    cardAnim.oncancel = finish;
  }

  function syncHistoryControl() {
    if (historyToggle) {
      const item = current();
      const viewingHistory = mode === 'history';
      historyToggle.hidden = !(viewingHistory ? pending().length : mode === 'pending' && item);
      historyToggle.setAttribute('aria-pressed', String(viewingHistory));
      historyToggle.dataset.tooltip = viewingHistory ? 'Back to Needs you' : 'History';
    }
  }

  function remember(key, draft) {
    if (draft?.text || draft?.stale) drafts.set(key, draft);
    else drafts.delete(key);
    try { sessionStorage.setItem(draftStorage, JSON.stringify([...drafts])); } catch {}
  }

  function choice(item) {
    const key = choiceTarget(item);
    const saved = drafts.get(draftKey(item, true));
    const same = saved?.target === key;
    return { key, text: same && !saved.stale ? saved.text : undefined, stale: same && !!saved.stale };
  }

  function selectChoice(item, text) {
    if (busy(item)) return;
    const target = choiceTarget(item);
    remember(draftKey(item, true), { target, text });
    for (const form of document.querySelectorAll('.question-choice-form')) {
      if (form.dataset.questionKey === target) updateQuestionChoiceForm(form, text, false);
    }
  }

  function composer() {
    const item = current();
    if (item?.itemKind !== 'user_question' || !item.canDecide || !canCompose()) return null;
    const key = draftKey(item, false);
    const target = decisionTarget(item);
    const saved = drafts.get(key);
    return { item, key: target, text: saved?.text ?? '', busy: busy(item),
      blocked: !!saved && (saved.target !== target || saved.stale), draftKey: key };
  }

  function reviewDraft(item, targetHost) {
    const saved = drafts.get(draftKey(item, false));
    const target = decisionTarget(item);
    if (!saved || (saved.target === target && !saved.stale)) {
      targetHost.replaceChildren();
      return;
    }
    targetHost.replaceChildren(node('p', 'board-inbox-meta', saved.target === target
      ? 'The question changed. Waiting for the current question.'
      : 'The question changed. Review it before using the previous draft.'));
    if (saved.target !== target) {
      targetHost.append(button('Use draft for this question', 'text-button', () => {
        if (!canCompose()) return;
        remember(draftKey(item, false), { target, text: saved.text });
        reviewDraft(item, targetHost);
        onChange();
        focusComposer();
      }));
    }
  }

  function questionControls(item, pending) {
    const group = node('div', 'board-inbox-question');
    const { key, text, stale } = choice(item);
    if (hasQuestionChoices(item.choices)) {
      group.append(questionChoiceForm({
        key, prompt: item.prompt, choices: item.choices, answer: text, busy: pending || stale,
        select: (answer) => selectChoice(item, answer),
        submit(answer) { void submit(item, 'answer', { text: answer }, { choiceAnswer: true }); },
      }));
    }
    const write = questionReplyButton(() => {
      if (!canCompose()) return;
      focusComposer();
    });
    write.disabled = pending;
    group.append(write);
    const needsReview = node('div', 'board-inbox-draft-review');
    reviewDraft(item, needsReview);
    group.append(needsReview);
    return group;
  }

  function syncTrigger() {
    const pending = bot ? count(bot) : 0;
    trigger.hidden = !pending;
    byId('needs-you-count').textContent = pending;
    trigger.setAttribute('aria-expanded', String(mode === 'pending'));
  }

  function renderPending() {
    syncTrigger();
    if (mode !== 'pending') return;
    const list = pending();
    const item = current();
    const index = list.indexOf(item);
    position.textContent = `${index + 1} of ${list.length}`;
    previous.disabled = index <= 0;
    next.disabled = index < 0 || index >= list.length - 1;
    syncHistoryControl();
    if (!item) return;
    const pendingWrite = busy(item);
    const questionState = item.itemKind === 'user_question' && [pendingWrite,
      drafts.get(draftKey(item, false))?.stale, drafts.get(draftKey(item, true))?.stale];
    const text = JSON.stringify([item, questionState]);
    if (rendered === text) {
      setCardBusy(host, pendingWrite);
      return;
    }
    rendered = text;
    const focused = host.contains(document.activeElement);
    const source = sourceAction(item, () => close({ focus: false }));
    host.replaceChildren(decisionCard(item, submit, source, () => questionControls(item, pendingWrite), pendingWrite));
    if (focused) panel.focus({ preventScroll: true });
  }

  function openLayer(nextMode, opener) {
    clearMorph();
    const wasClosed = !mode;
    const pillRect = wasClosed && caption && !caption.hidden ? caption.getBoundingClientRect() : null;
    if (!mode) origin = opener ?? trigger;
    mode = nextMode;
    rendered = null;
    layer.hidden = false;
    transcript.inert = true;
    title.textContent = mode === 'pending' ? 'Needs you' : 'History';
    for (const control of [position, next, previous]) control.hidden = mode === 'history';
    message.textContent = '';
    panel.scrollTop = 0;
    return pillRect;
  }

  function focusAfterOpen() {
    if (composer()) {
      focusComposer();
    } else {
      panel.focus({ preventScroll: true });
    }
  }

  function open(opener, targetItem) {
    const list = pending();
    const item = targetItem ? list.find((entry) => itemKey(entry) === itemKey(targetItem)) : list[0];
    if (!item) return;
    if (mode === 'pending' && itemKey(item) === selected) return;
    selected = itemKey(item);
    const pillRect = openLayer('pending', opener);
    renderPending();
    onChange();
    if (pillRect) animateOpenFromPill(pillRect);
    focusAfterOpen();
  }

  function openItem(item, opener) {
    open(opener, item);
  }

  async function openHistory(opener) {
    if (!bot) return;
    historyRequest?.abort();
    const pillRect = openLayer('history', opener);
    syncTrigger();
    syncHistoryControl();
    host.replaceChildren();
    onChange();
    if (pillRect) animateOpenFromPill(pillRect);
    panel.focus({ preventScroll: true });
    message.textContent = 'Loading…';
    const request = historyRequest = new AbortController();
    const sameView = () => request === historyRequest && mode === 'history';
    try {
      const { decisions } = await api(`inbox/history?${new URLSearchParams({ bot })}`, undefined, { signal: request.signal });
      if (!sameView()) return;
      const cards = decisions.map(historyCard);
      host.replaceChildren(...cards);
      message.textContent = decisions.length ? '' : 'No answered questions, secret requests or approvals yet.';
      animateHistoryCards(cards);
    } catch {
      if (!sameView()) return;
      message.textContent = 'Couldn’t load history.';
      host.append(button('Try again', 'text-button', () => { void openHistory(); }));
    }
  }

  function dismiss({ animate = false } = {}) {
    const cardRect = animate && mode && panel ? panel.getBoundingClientRect() : null;
    const pillRect = animate && mode && caption && !caption.hidden ? caption.getBoundingClientRect() : null;
    mode = null;
    selected = rendered = null;
    historyRequest?.abort();
    historyRequest = null;
    host?.getAnimations?.().forEach((anim) => anim.cancel());
    layer.hidden = true;
    transcript.inert = false;
    host.replaceChildren();
    message.textContent = '';
    syncTrigger();
    if (animate && cardRect && pillRect) animateCloseToPill(cardRect, pillRect);
  }

  function close({ focus = true, animate = true } = {}) {
    if (!mode) return;
    const opener = origin;
    dismiss({ animate });
    onChange();
    if (!focus) return;
    if (opener?.offsetParent) opener.focus({ preventScroll: true });
    else if (!trigger.hidden) trigger.focus({ preventScroll: true });
    else focusComposer();
  }

  function move(direction) {
    const list = pending();
    const index = list.indexOf(current());
    if (!list[index + direction]) return;
    const focused = document.activeElement;
    selected = itemKey(list[index + direction]);
    message.textContent = '';
    panel.scrollTop = 0;
    renderPending();
    animateStep(direction);
    onChange();
    if (focused?.disabled) panel.focus({ preventScroll: true });
  }

  function apply(nextItems) {
    const old = pending().indexOf(current());
    items = nextItems;
    if (mode === 'pending' && !current()) {
      const list = pending();
      const nextEntry = list[Math.max(0, Math.min(old, list.length - 1))];
      if (!nextEntry) {
        close({ focus: panel.contains(document.activeElement) || document.activeElement?.disabled });
        return;
      }
      selected = itemKey(nextEntry);
    }
    renderPending();
    onChange();
  }

  async function refresh() {
    const stamp = ++epoch;
    let latest;
    try {
      ({ items: latest } = await api('inbox'));
    } catch (error) {
      if (stamp !== epoch) return;
      if (error.status === 401 || error.status === 403) apply([]);
      else onRefreshError();
      return;
    }
    if (stamp !== epoch || !Array.isArray(latest)) return;
    if (JSON.stringify(items) !== JSON.stringify(latest)) apply(latest);
  }

  function setContext(botId, viewerId) {
    if (viewer !== viewerId) {
      viewer = viewerId;
      items = [];
      epoch++;
      clearMorph();
      dismiss({ animate: false });
    }
    if (bot !== botId) {
      bot = botId;
      clearMorph();
      dismiss({ animate: false });
    }
    syncTrigger();
  }

  const errors = {
    request_changed: 'The request changed. Review the current request before responding again.',
    already_closed: 'This request was already answered or closed.',
    access_denied: 'You don’t have permission to respond to this request.',
    decision_conflict: 'Flock could not apply your response. Check it and try again.',
    invalid_input: 'Check your response and try again.',
  };

  async function submit(item, decision, data = {}, { inline = false, choiceAnswer = false, conversation } = {}) {
    const active = conversation ? conversation.active
      : inline ? items.find((entry) => itemKey(entry) === itemKey(item)) : current();
    const fingerprint = decisionTarget(item);
    if (!active?.canDecide || busy(item)) return;
    const matchesActiveTarget = decisionTarget(active) === fingerprint;
    if (!matchesActiveTarget || (!conversation && !item.canDecide)) return;
    if (savesFlockRule(decision) && active.canSaveRule !== true) return;
    const owner = viewer;
    const save = saveKey(item);
    const textKey = draftKey(item, false);
    const choiceKey = draftKey(item, true);
    const answer = decision === 'answer' ? choiceAnswer ? choiceKey : textKey : undefined;
    const saved = drafts.get(answer);
    const otherKey = choiceAnswer ? textKey : choiceKey;
    const otherDraft = drafts.get(otherKey);
    const isCurrent = () => owner === viewer && current() && decisionTarget(current()) === fingerprint;
    const target = Object.fromEntries(identity.map((field) => [field, item[field]]));
    const writes = inFlight() ?? new Map();
    writes.set(save, item.botId);
    saving.set(owner, writes);
    try {
      if (conversation) conversation.start();
      else message.textContent = 'Sending…';
      renderPending();
      onChange();
      let failure;
      try {
        const useOperation = !item.itemKind || decision === 'cancel';
        const response = await (useOperation
          ? api('operations', { ...data, ...target, operation: decision })
          : api('inbox/decide', { ...target, decision, data }));
        if (response?.accepted !== true) throw new Error('decision_unconfirmed');
      } catch (error) {
        failure = error;
      }
      if (answer && drafts.get(answer) === saved) {
        if (!failure) {
          remember(answer);
          if (drafts.get(otherKey) === otherDraft) remember(otherKey);
        } else if (failure.message === 'request_changed') {
          remember(answer, { target: choiceAnswer ? choiceTarget(item) : fingerprint, text: data.text, stale: true });
        }
      }
      if (owner !== viewer) return;
      if (isCurrent()) message.textContent = failure
        ? errors[failure.message] ?? 'Couldn’t confirm your response. Check the request before trying again.' : '';
      if (!failure) {
        epoch++;
        const remaining = items.filter((entry) => conversation
          ? saveKey(entry) !== save || entry.targetRevision !== item.targetRevision || entry.targetGeneration !== item.targetGeneration
          : decisionTarget(entry) !== fingerprint);
        if (remaining.length !== items.length) apply(remaining);
      }
      if (conversation) {
        await conversation.finish(failure, refresh);
      } else {
        if (!failure) onAccepted(item, { decision });
        else if (inline) onAccepted(item, { error: errors[failure.message] ?? 'Couldn’t confirm your response.' });
        await Promise.all([refresh(), refreshConversation(item)]);
      }
    } finally {
      writes.delete(save);
      if (!writes.size) saving.delete(owner);
      if (owner === viewer) {
        renderPending();
        onChange();
      }
    }
  }

  trigger.addEventListener('click', () => mode === 'pending' ? close() : open(trigger));
  byId('decision-close').addEventListener('click', () => close());
  historyToggle?.addEventListener('click', () => {
    if (mode === 'history') open(historyToggle);
    else void openHistory(historyToggle);
  });
  byId('decision-shade').addEventListener('click', () => close());
  previous.addEventListener('click', () => move(-1));
  next.addEventListener('click', () => move(1));
  // Double-tap Option (Alt Alt) toggles the Needs you card, matching the reference shortcut.
  let lastAltTap = 0;
  let altInterrupted = false;
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Alt' && !event.repeat && !event.ctrlKey && !event.metaKey && !event.shiftKey) {
      altInterrupted = false;
      return;
    }
    if (event.key !== 'Alt') altInterrupted = true;
    if (event.key === 'Escape' && mode && canDismiss() && !event.defaultPrevented && !event.isComposing && event.keyCode !== 229 &&
        !document.querySelector('dialog[open]')) {
      event.preventDefault();
      close();
    }
  });
  document.addEventListener('keyup', (event) => {
    if (event.key !== 'Alt') return;
    if (altInterrupted || event.isComposing || document.querySelector('dialog[open]')) {
      lastAltTap = 0;
      return;
    }
    const now = performance.now();
    if (now - lastAltTap <= 360) {
      lastAltTap = 0;
      if (mode) close();
      else if (bot && count(bot)) open(trigger);
    } else {
      lastAltTap = now;
    }
  });

  return {
    setContext, refresh, count, itemsFor: scoped, pendingItemsFor: pending, open, openItem, openHistory, close, isOpen, composer,
    busy, choice, selectChoice,
    pendingKeys: () => [...inFlight()?.keys() ?? []],
    pendingForBot: (botId) => [...inFlight()?.values() ?? []].includes(botId),
    submitConversation(item, decision, data, conversation) {
      return submit(item, decision, data, { conversation, choiceAnswer: decision === 'answer' });
    },
    inlinePolicyCard(item) {
      const card = decisionCard(item, (entry, decision, data) => submit(entry, decision, data, { inline: true }),
        null, null, busy(item));
      card.classList.add('policy-inline-card');
      return card;
    },
    edit(text) {
      const draft = composer();
      if (!draft || draft.busy || draft.text === text) return;
      remember(draft.draftKey, { target: draft.key, text });
      const review = host.querySelector('.board-inbox-draft-review');
      if (review) reviewDraft(draft.item, review);
    },
    submitAnswer() {
      const draft = composer();
      if (draft && !draft.blocked && draft.text.trim()) {
        return submit(draft.item, 'answer', { text: draft.text });
      }
    },
  };
}
