import { hasQuestionChoices, questionChoiceForm, updateQuestionChoiceForm } from './question-choices.js';
import { actionLabel, approvalSummary } from './action-labels.mjs';

const labels = {
  user_question: 'Question', extra_run: 'One more run', deadline: 'Deadline extension',
  model_budget: 'Another model call', conversation_model_budget: 'Another model call',
  unknown_delivery: 'Unconfirmed message to T3',
  protected_input: 'Secret value', pr_review: 'Pull request review',
  pr_reconciliation: 'Unconfirmed pull request',
  answer: 'Answered', approve: 'Approved', deny: 'Denied', supply: 'Supplied',
  extend: 'Extended', reconcile: 'Checked again', request_changes: 'Changes requested',
  link_pr: 'Pull request linked', reject_pr: 'Rejected',
  policy_approval: 'Approval', allow_once: 'Allowed once', always_allow: 'Always allowed',
  allow_next_time: 'Allow rule saved',
};
const actions = {
  extra_run: [['approve', 'Approve one run']],
  deadline: [['extend', 'Extend deadline']],
  model_budget: [['approve', 'Approve one call']],
  conversation_model_budget: [['approve', 'Approve one call']],
  unknown_delivery: [['reconcile', 'Check T3 again']],
  protected_input: [['supply', 'Save secret']],
  pr_reconciliation: [['reject_pr', 'Reject']],
  policy_approval: [['allow_once', 'Allow once'], ['always_allow', 'Always allow'], ['deny', 'Deny']],
  pr_review: [['request_changes', 'Request changes'], ['approve', 'Approve review']],
};
const identity = ['botId', 'itemKind', 'itemId', 'waitId', 'taskId', 'targetRevision', 'targetGeneration'];
const draftStorage = 'flock-inbox-answer-drafts';

const svgIcons = {
  pr: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.85" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="6" cy="6" r="2.5"/><circle cx="18" cy="18" r="2.5"/><circle cx="6" cy="18" r="2.5"/><path d="M6 8.5v7M18 15.5V10a3 3 0 0 0-3-3h-3.5m2-2-2 2 2 2"/></svg>',
  external: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M14 5h5v5M10 14 19 5M19 13v5a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1h5"/></svg>',
  copy: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>',
  check: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m5 12.5 4.2 4.2L19 7"/></svg>',
  history: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 13.5 6.2 6a2 2 0 0 1 1.9-1.5h7.8A2 2 0 0 1 17.8 6l2.2 7.5V18a1.5 1.5 0 0 1-1.5 1.5h-13A1.5 1.5 0 0 1 4 18v-4.5Z"/><path d="M4 13.5h4.5l1 2h5l1-2H20"/></svg>',
};

const itemKey = (item) => JSON.stringify([item.botId, item.itemKind, item.itemId]);
const decisionTarget = (item) => JSON.stringify([...identity.map((field) => item[field]), item.request, item.prompt, item.choices, item.status]);

function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text) node.textContent = text;
  if (className) node.className = className;
  return node;
}
function iconSpan(markup) {
  const host = document.createElement('span');
  host.innerHTML = markup;
  return host.firstElementChild;
}
function labeled(text, control) {
  const label = element('label', text);
  label.append(control);
  return label;
}
const clockFormat = new Intl.DateTimeFormat('en', { hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
const dayFormat = new Intl.DateTimeFormat('en', { month: 'short', day: 'numeric' });
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
  return new Date(seconds * 1000).toLocaleDateString();
}
function parsePrInfo(raw) {
  try {
    const url = new URL(raw);
    if (url.protocol !== 'https:' && url.protocol !== 'http:') return null;
    const displayUrl = `${url.host}${url.pathname}`.replace(/\/+$/, '');
    const match = url.pathname.match(/\/([^/]+)\/pull\/(\d+)/i) || url.pathname.match(/\/([^/]+)\/merge_requests\/(\d+)/i);
    const repoTag = match ? `${match[1]}#${match[2]}` : url.host;
    return { href: url.href, displayUrl, repoTag, host: url.host };
  } catch {
    return null;
  }
}
export function pullRequestLink(raw, title) {
  const info = parsePrInfo(raw);
  if (!info) return element('span', title);
  const link = element('a', title);
  link.href = info.href;
  link.target = '_blank';
  link.rel = 'noopener noreferrer';
  return link;
}
function historyCard(entry) {
  const article = element('article', '', 'board-inbox-card history-card');
  const approval = entry.itemKind === 'policy_approval' && entry.action;
  const title = approval ? actionLabel(entry.action) : entry.requestTitle || labels[entry.itemKind] || 'Request';
  const heading = element('h3', title);
  if (approval && entry.target) heading.append(element('span', entry.target, 'board-inbox-target'));
  article.append(heading);
  // Name the kind of item only when the title doesn't already say it, then who answered and when.
  const about = approval ? entry.requestTitle : entry.requestTitle && labels[entry.itemKind];
  if (about) article.append(element('p', about, 'history-about'));
  const by = entry.decidedByYou ? 'you' : entry.decidedBy;
  article.append(element('p', `${labels[entry.decision] ?? 'Answered'} by ${by} · ${time(entry.decidedAt)}`, 'board-inbox-meta'));
  return article;
}
function button(text, className, click) {
  const node = element('button', text, className);
  node.type = 'button';
  node.addEventListener('click', click);
  return node;
}
function prPreviewCard(rawUrl, title) {
  const info = parsePrInfo(rawUrl);
  if (!info) return element('span', title || 'Pull request', 'board-inbox-meta');
  const box = element('div', '', 'board-inbox-pr-card');
  const head = element('div', '', 'board-inbox-pr-head');
  const link = pullRequestLink(info.href, title || 'Pull request');
  link.className = 'board-inbox-pr-link';
  const tools = element('div', '', 'board-inbox-pr-tools');
  const openBtn = element('a', '', 'board-inbox-pr-tool');
  openBtn.href = info.href;
  openBtn.target = '_blank';
  openBtn.rel = 'noopener noreferrer';
  openBtn.setAttribute('aria-label', 'Open pull request in new tab');
  openBtn.title = 'Open pull request';
  openBtn.append(iconSpan(svgIcons.external));
  const copyBtn = element('button', '', 'board-inbox-pr-tool');
  copyBtn.type = 'button';
  copyBtn.setAttribute('aria-label', 'Copy pull request link');
  copyBtn.title = 'Copy link';
  copyBtn.append(iconSpan(svgIcons.copy));
  let copyTimer = null;
  copyBtn.addEventListener('click', async (event) => {
    event.preventDefault();
    event.stopPropagation();
    try {
      await navigator.clipboard?.writeText(info.href);
      clearTimeout(copyTimer);
      copyBtn.classList.remove('is-copied');
      copyBtn.replaceChildren(iconSpan(svgIcons.check));
      copyBtn.classList.add('is-copied');
      copyTimer = setTimeout(() => {
        copyBtn.classList.remove('is-copied');
        copyBtn.replaceChildren(iconSpan(svgIcons.copy));
      }, 1200);
    } catch {}
  });
  tools.append(openBtn, copyBtn);
  head.append(link, tools);

  const meta = element('div', '', 'board-inbox-pr-meta');
  meta.append(iconSpan(svgIcons.pr), element('span', info.repoTag));
  if (info.repoTag !== info.host) {
    meta.append(element('span', '·', 'board-inbox-submeta-sep'), element('span', info.host));
  }
  box.append(head, meta);
  return box;
}
function decisionCard(item, submit, source, question, saving) {
  const article = element('article', '', 'board-inbox-card');
  const prInfo = item.prUrl ? parsePrInfo(item.prUrl) : null;

  const titleGroup = element('div', '', 'board-inbox-title-group');
  const approval = item.itemKind === 'policy_approval' && item.action;
  titleGroup.append(element('h3', approval ? actionLabel(item.action) : item.requestTitle));

  const submeta = element('div', '', 'board-inbox-submeta');
  const parts = [];
  // The pull request card below carries the link; the header only names the kind of decision.
  if (item.itemKind !== 'policy_approval' && labels[item.itemKind]) {
    parts.push(element('span', labels[item.itemKind]));
  }
  if (approval && item.target) parts.push(element('span', item.target, 'board-inbox-target'));
  if (item.status === 'expired') parts.push(element('span', 'Expired'));
  const rel = relativeTime(item.createdAt);
  if (rel) parts.push(element('span', rel));
  if (source) parts.push(source);
  parts.forEach((part, idx) => {
    if (idx > 0) submeta.append(element('span', '·', 'board-inbox-submeta-sep'));
    submeta.append(part);
  });
  if (parts.length) titleGroup.append(submeta);
  article.append(titleGroup);

  const bodyLines = [];
  if (item.request && item.request !== item.requestTitle) bodyLines.push(item.request);
  const prompt = approval ? approvalSummary(item.prompt, item) : item.prompt;
  if (prompt && prompt !== item.requestTitle && prompt !== item.request) bodyLines.push(prompt);
  // A bare pull request URL in the text repeats the card, so drop that line.
  const text = bodyLines.join('\n').split('\n')
    .filter((line) => !prInfo || ![item.prUrl, prInfo.href].includes(line.trim())).join('\n').trim();
  if (text) article.append(element('p', text, 'board-inbox-body'));

  if (item.prUrl) {
    article.append(prPreviewCard(item.prUrl, item.prTitle || 'Pull request'));
  }
  for (const pr of item.candidates || []) article.append(prPreviewCard(pr.url, `Pull request #${pr.prNumber}`));

  if (!item.canDecide) {
    if (item.itemKind !== 'policy_approval') article.append(element('p', 'View only', 'board-inbox-meta'));
    return article;
  }
  if (item.itemKind === 'user_question') {
    article.append(question());
    return article;
  }
  const form = element('form', '', 'board-inbox-form');
  const inputs = {};
  const buttons = element('div', '', 'board-inbox-actions');
  if (item.itemKind === 'deadline') {
    inputs.deadline = element('input');
    inputs.deadline.type = 'datetime-local';
    inputs.deadline.required = true;
    form.append(labeled('New deadline', inputs.deadline));
  } else if (item.itemKind === 'protected_input') {
    inputs.value = element('input');
    inputs.value.type = 'password';
    inputs.value.autocomplete = 'off';
    inputs.value.required = true;
    form.append(labeled('Secret value', inputs.value));
  }
  for (const pr of item.candidates || []) {
    const btn = element('button', `Link #${pr.prNumber}`);
    btn.type = 'submit';
    btn.value = 'link_pr';
    btn.dataset.prNumber = pr.prNumber;
    buttons.append(btn);
  }
  const choices = item.itemKind === 'policy_approval' && item.status === 'expired'
    ? [['allow_next_time', 'Always allow next time']]
    : actions[item.itemKind];
  for (const [decision, title] of choices) {
    const style = item.itemKind === 'pr_review' ? (decision === 'approve' ? 'primary' : 'secondary')
      // One default per card: "Allow once" leads; the lasting "Always allow" stays a deliberate second choice.
      : (['deny', 'reject_pr', 'always_allow'].includes(decision) ? 'secondary' : 'primary');
    const btn = element('button', title, style);
    btn.value = decision;
    btn.type = 'submit';
    buttons.append(btn);
  }
  form.append(buttons);
  function setBusy(busy) {
    for (const input of form.elements) input.disabled = busy;
  }
  setBusy(Boolean(saving));
  saving?.then(() => setBusy(false), () => setBusy(false));
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (!event.submitter || event.submitter.disabled) return;
    const data = Object.fromEntries(Object.entries(inputs).map(([field, input]) => [field, input.value]));
    if (event.submitter.dataset.prNumber) data.prNumber = Number(event.submitter.dataset.prNumber);
    if (data.deadline) data.deadline = Math.floor(new Date(data.deadline).getTime() / 1000);
    setBusy(true);
    try { await submit(item, event.submitter.value, data); }
    finally { setBusy(false); }
  });
  article.append(form);
  return article;
}

export function mountBoardInbox({
  api, sourceAction, onChange, onAccepted, onRefreshError, canCompose, focusComposer, canDismiss = () => true,
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
  const more = byId('decision-more');
  const moreList = byId('decision-more-list');
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
  const current = () => mode === 'pending' ? scoped().find((item) => itemKey(item) === selected) : undefined;
  const isOpen = () => !!mode;
  const count = (botId) => scoped(botId).length;
  const saveKey = (item) => JSON.stringify([viewer, itemKey(item)]);
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
    const overlay = element('div', '', 'decision-morph-overlay');
    overlay.setAttribute('aria-hidden', 'true');
    const ghostShade = element('div', '', 'decision-morph-shade');
    const ghostCard = element('div', '', 'decision-morph-card');
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

  function closeMoreMenu() {
    if (more?.open) more.open = false;
  }

  function syncMoreMenu() {
    if (!more || !moreList) return;
    const actionsList = [];
    const item = current();
    if (mode === 'pending' && item) {
      const prInfo = item.prUrl ? parsePrInfo(item.prUrl) : null;
      if (prInfo) {
        const openLink = element('a', '', 'decision-more-item');
        openLink.href = prInfo.href;
        openLink.target = '_blank';
        openLink.rel = 'noopener noreferrer';
        openLink.append(iconSpan(svgIcons.external), element('span', 'Open pull request'));
        openLink.addEventListener('click', closeMoreMenu);
        actionsList.push(openLink);

        const copyBtn = element('button', '', 'decision-more-item');
        copyBtn.type = 'button';
        copyBtn.append(iconSpan(svgIcons.copy), element('span', 'Copy pull request link'));
        copyBtn.addEventListener('click', () => {
          void navigator.clipboard?.writeText(prInfo.href);
          closeMoreMenu();
        });
        actionsList.push(copyBtn);
      }
      const histBtn = element('button', '', 'decision-more-item');
      histBtn.type = 'button';
      histBtn.append(iconSpan(svgIcons.history), element('span', 'History'));
      histBtn.addEventListener('click', () => {
        closeMoreMenu();
        void openHistory(origin);
      });
      actionsList.push(histBtn);
    } else if (mode === 'history' && scoped().length) {
      const pendingBtn = element('button', '', 'decision-more-item');
      pendingBtn.type = 'button';
      pendingBtn.append(iconSpan(svgIcons.history), element('span', 'Needs you'));
      pendingBtn.addEventListener('click', () => {
        closeMoreMenu();
        open(origin);
      });
      actionsList.push(pendingBtn);
    }
    more.hidden = !actionsList.length;
    moreList.replaceChildren(...actionsList);
  }

  function remember(key, draft) {
    if (draft?.text || draft?.stale) drafts.set(key, draft);
    else drafts.delete(key);
    try { sessionStorage.setItem(draftStorage, JSON.stringify([...drafts])); } catch {}
  }

  function composer() {
    const item = current();
    if (item?.itemKind !== 'user_question' || !item.canDecide || !canCompose()) return null;
    const key = draftKey(item, false);
    const target = decisionTarget(item);
    const saved = drafts.get(key);
    return { item, key: target, text: saved?.text ?? '', busy: saving.has(saveKey(item)),
      blocked: !!saved && (saved.target !== target || saved.stale), draftKey: key };
  }

  function reviewReply() {
    const item = current();
    const id = item?.conversationEventId;
    if (item?.itemKind !== 'pr_review' || !item.canDecide || !canCompose() || !Number.isSafeInteger(id) || id <= 0) return null;
    return { kind: 'message', messageId: id, summary: item.requestTitle };
  }

  function reviewDraft(item, targetHost) {
    const saved = drafts.get(draftKey(item, false));
    const target = decisionTarget(item);
    if (!saved || (saved.target === target && !saved.stale)) {
      targetHost.replaceChildren();
      return;
    }
    targetHost.replaceChildren(element('p', saved.target === target
      ? 'The question changed. Waiting for the current question.'
      : 'The question changed. Review it before using the previous draft.', 'board-inbox-meta'));
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

  function questionControls(item, busy) {
    const group = element('div', '', 'board-inbox-question');
    const target = decisionTarget(item);
    const saved = drafts.get(draftKey(item, true));
    const same = saved?.target === target;
    let selectedAnswer = same && !saved.stale ? saved.text : undefined;
    const stale = same && saved.stale;
    if (hasQuestionChoices(item.choices)) {
      const form = questionChoiceForm({
        key: target, prompt: item.prompt, choices: item.choices, answer: selectedAnswer, busy: busy || stale,
        select(answer) {
          selectedAnswer = answer;
          remember(draftKey(item, true), { target, text: answer });
          updateQuestionChoiceForm(form, selectedAnswer, saving.has(saveKey(item)));
        },
        submit(answer) { void submit(item, 'answer', { text: answer }, draftKey(item, true)); },
      });
      group.append(form);
    }
    const write = button('Write an answer', 'text-button question-reply', () => {
      if (!canCompose()) return;
      focusComposer();
    });
    write.disabled = busy;
    group.append(write);
    const needsReview = element('div', '', 'board-inbox-draft-review');
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
    const list = scoped();
    const item = current();
    const index = list.indexOf(item);
    position.textContent = `${index + 1}/${list.length}`;
    previous.disabled = index <= 0;
    next.disabled = index < 0 || index >= list.length - 1;
    syncMoreMenu();
    if (!item) return;
    const pendingWrite = saving.get(saveKey(item));
    const questionState = item.itemKind === 'user_question' && [!!pendingWrite,
      drafts.get(draftKey(item, false))?.stale, drafts.get(draftKey(item, true))?.stale];
    const text = JSON.stringify([item, questionState]);
    if (rendered === text) return;
    rendered = text;
    const focused = host.contains(document.activeElement);
    const source = sourceAction(item, () => close({ focus: false }));
    host.replaceChildren(decisionCard(item, submit, source, () => questionControls(item, !!pendingWrite), pendingWrite));
    if (focused) panel.focus({ preventScroll: true });
  }

  function openLayer(nextMode, opener) {
    clearMorph();
    closeMoreMenu();
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
    if (composer() || reviewReply()) {
      focusComposer();
    } else {
      panel.focus({ preventScroll: true });
    }
  }

  function open(opener, targetItem) {
    const list = scoped();
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
    syncMoreMenu();
    host.replaceChildren();
    onChange();
    if (pillRect) animateOpenFromPill(pillRect);
    panel.focus({ preventScroll: true });
    message.textContent = 'Loading…';
    const request = historyRequest = new AbortController();
    const sameView = () => request === historyRequest && mode === 'history';
    try {
      const { decisions } = await api(`inbox/history?${new URLSearchParams({ bot })}`, undefined, request.signal);
      if (!sameView()) return;
      const cards = decisions.map(historyCard);
      host.replaceChildren(...cards);
      message.textContent = decisions.length ? '' : 'Nothing here yet. Questions and approvals you respond to will appear here.';
      animateHistoryCards(cards);
    } catch {
      if (!sameView()) return;
      message.textContent = 'Could not load history.';
      host.append(button('Try again', 'text-button', () => { void openHistory(); }));
    }
  }

  function dismiss({ animate = false } = {}) {
    const cardRect = animate && mode && panel ? panel.getBoundingClientRect() : null;
    const pillRect = animate && mode && caption && !caption.hidden ? caption.getBoundingClientRect() : null;
    closeMoreMenu();
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
    const list = scoped();
    const index = list.indexOf(current());
    if (!list[index + direction]) return;
    const focused = document.activeElement;
    closeMoreMenu();
    selected = itemKey(list[index + direction]);
    message.textContent = '';
    panel.scrollTop = 0;
    renderPending();
    animateStep(direction);
    onChange();
    if (focused?.disabled) panel.focus({ preventScroll: true });
  }

  function apply(nextItems) {
    const old = scoped().indexOf(current());
    items = nextItems;
    if (mode === 'pending' && !current()) {
      const list = scoped();
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
    access_denied: 'You do not have permission to respond to this request.',
    decision_conflict: 'Flock could not apply your response. Check it and try again.',
    invalid_input: 'Check your response and try again.',
  };

  async function submit(item, decision, data, answer, inline = false) {
    const save = saveKey(item);
    const active = inline ? items.find((entry) => itemKey(entry) === itemKey(item)) : current();
    if (!item.canDecide || saving.has(save) || !active || decisionTarget(active) !== decisionTarget(item)) return;
    const owner = viewer;
    const target = Object.fromEntries(identity.map((field) => [field, item[field]]));
    const fingerprint = decisionTarget(item);
    const saved = drafts.get(answer);
    const textKey = draftKey(item, false);
    const choiceKey = draftKey(item, true);
    const isCurrent = () => owner === viewer && current() && decisionTarget(current()) === fingerprint;
    message.textContent = 'Sending…';
    const operation = api('inbox/decide', { ...target, decision, data });
    saving.set(save, operation);
    renderPending();
    onChange();
    try {
      const response = await operation;
      if (!response.accepted) throw new Error('decision_unconfirmed');
      if (answer) {
        if (drafts.get(answer) === saved) {
          remember(textKey);
          remember(choiceKey);
        }
      }
      if (owner !== viewer) return;
      epoch++;
      if (isCurrent()) message.textContent = '';
      apply(items.filter((entry) => decisionTarget(entry) !== fingerprint));
      onAccepted(item, { decision });
      void refresh();
    } catch (error) {
      if (answer && error.message === 'request_changed' && drafts.get(answer) === saved) {
        remember(answer, { target: fingerprint, text: data.text, stale: true });
      }
      if (inline && owner === viewer) onAccepted(item, { error: errors[error.message] ?? 'Could not confirm your response.' });
      if (isCurrent()) message.textContent = errors[error.message] ?? 'Could not confirm your response. Check the request before trying again.';
      if (owner === viewer) await refresh();
    } finally {
      saving.delete(save);
      if (owner === viewer) {
        renderPending();
        onChange();
      }
    }
  }

  trigger.addEventListener('click', () => mode === 'pending' ? close() : open(trigger));
  byId('decision-close').addEventListener('click', () => close());
  byId('decision-shade').addEventListener('click', () => close());
  previous.addEventListener('click', () => move(-1));
  next.addEventListener('click', () => move(1));
  document.addEventListener('click', (event) => {
    if (more?.open && !more.contains(event.target)) closeMoreMenu();
  });

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
      if (more?.open) {
        closeMoreMenu();
        more.querySelector('summary')?.focus({ preventScroll: true });
      } else {
        close();
      }
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
    setContext, refresh, count, itemsFor: (botId) => scoped(botId), open, openItem, openHistory, close, isOpen, composer, reviewReply,
    inlinePolicyCard(item) {
      const card = decisionCard(item, (entry, decision, data) => submit(entry, decision, data, undefined, true),
        null, null, saving.get(saveKey(item)));
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
        return submit(draft.item, 'answer', { text: draft.text }, draft.draftKey);
      }
    },
  };
}
