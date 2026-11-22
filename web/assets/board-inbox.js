import { hasQuestionChoices, questionChoiceForm, questionReplyButton, updateQuestionChoiceForm } from './question-choices.js';

const labels = {
  user_question: 'Question', extra_run: 'Extra run', deadline: 'Deadline extension',
  model_budget: 'Extra model call', conversation_model_budget: 'Extra conversation model call',
  unknown_delivery: 'Delivery confirmation', grant_request: 'Access request',
  protected_input: 'Protected input', bot_change: 'Bot change', pr_review: 'Pull request review',
  pr_reconciliation: 'Pull request reconciliation',
  answer: 'Answered', approve: 'Approved', deny: 'Denied', supply: 'Supplied',
  extend: 'Extended', reconcile: 'Reconciliation requested', request_changes: 'Changes requested',
  link_pr: 'Pull request linked', reject_pr: 'Operation rejected',
};
const actions = {
  extra_run: [['approve', 'Approve one run']],
  deadline: [['extend', 'Extend deadline']],
  model_budget: [['approve', 'Approve one call']],
  conversation_model_budget: [['approve', 'Approve one call']],
  unknown_delivery: [['reconcile', 'Reconcile delivery']],
  grant_request: [['approve', 'Approve access'], ['deny', 'Deny']],
  protected_input: [['supply', 'Save protected value']],
  bot_change: [['approve', 'Approve change'], ['deny', 'Deny']],
  pr_reconciliation: [['reject_pr', 'Reject operation']],
  pr_review: [['approve', 'Approve review'], ['request_changes', 'Request changes']],
};
const identity = ['botId', 'itemKind', 'itemId', 'waitId', 'taskId', 'targetRevision', 'targetGeneration'];
const draftStorage = 'flock-inbox-answer-drafts';

function answerKey(item) {
  return JSON.stringify([item.botId, item.taskId]);
}
function choiceKey(item) {
  return JSON.stringify([item.botId, item.taskId, 'choice']);
}
function decisionTarget(item) {
  return JSON.stringify([...identity.map((field) => item[field]), item.request, item.prompt, item.choices]);
}
function answerFocus(input) {
  return { key: input.dataset.answerKey, selection: [input.selectionStart, input.selectionEnd, input.selectionDirection] };
}

function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text) node.textContent = text;
  if (className) node.className = className;
  return node;
}
function labeled(text, control) {
  const label = element('label', text);
  label.append(control);
  return label;
}
function time(seconds) {
  return new Date(seconds * 1000).toLocaleString();
}
function prLink(raw, title) {
  try {
    const url = new URL(raw);
    if (url.protocol !== 'https:' && url.protocol !== 'http:') return element('span', title);
    const link = element('a', title);
    link.href = url.href;
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
    return link;
  } catch {
    return element('span', title);
  }
}
function card(title, meta) {
  const article = element('article', '', 'board-inbox-card');
  article.append(element('p', meta, 'board-inbox-meta'), element('h3', title));
  return article;
}
function questionForms(item, submit, drafts, remember, saving, focus) {
  const key = answerKey(item);
  const selectionKey = choiceKey(item);
  const target = decisionTarget(item);
  const draft = drafts.get(key);
  const savedChoice = drafts.get(selectionKey);
  const choiceStale = savedChoice?.target === target && savedChoice.stale;
  let selected = savedChoice?.target === target && !choiceStale ? savedChoice.text : undefined;
  let draftTarget = draft?.target ?? target;
  let stale = draft?.stale;
  let busy = Boolean(saving);
  const changed = draftTarget !== target;
  const group = element('div', '', 'board-inbox-question');
  const form = element('form', '', 'board-inbox-form');
  const input = element('textarea');
  input.dataset.answerKey = key;
  input.rows = 2;
  input.maxLength = 8000;
  input.required = true;
  input.value = draft?.text ?? '';
  const send = element('button', 'Send answer');
  send.type = 'submit';
  let review;
  let choices;
  let reply;
  const saveAnswer = () => remember(key, { target: draftTarget, text: input.value, stale });
  const useCurrentQuestion = () => {
    draftTarget = target;
    stale = false;
    saveAnswer();
    review.remove();
    review = undefined;
    setBusy(busy);
  };
  input.addEventListener('input', () => {
    if (draftTarget !== target && input.value !== draft?.text) useCurrentQuestion();
    else saveAnswer();
  });
  if (changed || stale) {
    review = element('div');
    review.append(element('p', changed
      ? 'The question changed. Review the current question before using this draft.'
      : 'The question changed. Refresh to load the current question.', 'board-inbox-meta'));
    if (changed) {
      const useDraft = element('button', 'Use draft for this question');
      useDraft.type = 'button';
      useDraft.addEventListener('click', () => {
        useCurrentQuestion();
        input.focus();
      });
      review.append(useDraft);
    }
    form.append(review);
  }
  if (hasQuestionChoices(item.choices)) {
    const actions = element('div', '', 'question-actions');
    form.hidden = !draft && focus?.key !== key;
    form.id = `inbox-answer-${item.waitId}`;
    send.textContent = 'Send reply';
    choices = questionChoiceForm({
      key: target, prompt: item.prompt, choices: item.choices, answer: selected, busy: busy || choiceStale,
      select(answer) {
        selected = answer;
        remember(selectionKey, { target, text: answer });
        updateQuestionChoiceForm(choices, selected, busy);
      },
      submit(answer) { void sendAnswer(answer, true); },
    });
    reply = questionReplyButton(() => {
      form.hidden = false;
      reply.setAttribute('aria-expanded', 'true');
      input.focus();
    });
    reply.setAttribute('aria-controls', form.id);
    reply.setAttribute('aria-expanded', String(!form.hidden));
    actions.append(choices, reply);
    group.append(actions);
    if (choiceStale) group.append(element('p', 'The question changed. Refresh to load the current question.', 'board-inbox-meta'));
  }
  const buttons = element('div', '', 'board-inbox-actions');
  buttons.append(send);
  form.append(labeled('Answer', input), buttons);
  group.append(form);
  function setBusy(value) {
    busy = value;
    for (const control of form.elements) control.disabled = busy;
    send.disabled = busy || Boolean(review);
    if (choices) updateQuestionChoiceForm(choices, selected, busy || choiceStale);
    if (reply) reply.disabled = busy;
  }
  async function sendAnswer(text, choice = false) {
    const active = document.activeElement;
    const focus = !choice && form.contains(active) ? { ...answerFocus(input), origin: active } : undefined;
    setBusy(true);
    try { await submit(item, 'answer', { text }, focus, choice); }
    finally { setBusy(false); }
  }
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    if (event.submitter === send && !send.disabled) void sendAnswer(input.value);
  });
  setBusy(busy);
  saving?.then(() => setBusy(false), () => setBusy(false));
  return group;
}
function decisionCard(item, submit, drafts, remember, saving, focus) {
  const article = card(item.requestTitle, `${item.botName} · ${labels[item.itemKind]}`);
  if (item.request && item.request !== item.requestTitle) article.append(element('p', item.request));
  article.append(element('p', item.prompt));
  if (item.prUrl) article.append(prLink(item.prUrl, item.prTitle));
  for (const pr of item.candidates || []) article.append(prLink(pr.url, `Pull request #${pr.prNumber}`));
  if (!item.canDecide) {
    article.append(element('p', 'View only', 'board-inbox-meta'));
    return article;
  }
  if (item.itemKind === 'user_question') {
    article.append(questionForms(item, submit, drafts, remember, saving, focus));
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
    form.append(labeled('Protected value', inputs.value));
  }
  for (const pr of item.candidates || []) {
    const button = element('button', `Link #${pr.prNumber}`);
    button.type = 'submit';
    button.value = 'link_pr';
    button.dataset.prNumber = pr.prNumber;
    buttons.append(button);
  }
  for (const [decision, title] of actions[item.itemKind]) {
    const button = element('button', title);
    button.value = decision;
    button.type = 'submit';
    buttons.append(button);
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

export function mountBoardInbox(api, sourceAction) {
  const byId = (id) => document.getElementById(id);
  const dialog = byId('board-inbox');
  const bots = byId('work-view-bot');
  const message = byId('work-view-message');
  const items = byId('work-view-items');
  const history = byId('work-view-history');
  let drafts = new Map();
  try { drafts = new Map(JSON.parse(sessionStorage.getItem(draftStorage))); } catch {}
  const saving = new Map();
  const views = {
    inbox: ['Decision inbox', 'items', 'No pending decisions.', (item, focus) =>
      decisionCard(item, submit, drafts, remember, saving.get(decisionTarget(item)), focus)],
    'inbox/history': ['Recent decisions', 'decisions', 'No recorded decisions.', (entry) => card(labels[entry.itemKind],
      `${entry.botName} · ${labels[entry.decision]} by ${entry.decidedBy} · ${time(entry.decidedAt)}`)],
  };
  let view;
  let version = 0;
  let availableBots;

  function remember(key, draft) {
    if (draft?.text || draft?.stale) drafts.set(key, draft);
    else drafts.delete(key);
    try { sessionStorage.setItem(draftStorage, JSON.stringify([...drafts])); } catch {}
  }

  async function submit(item, decision, data, focus, choice = false) {
    const displayed = version;
    const key = decision === 'answer' ? (choice ? choiceKey(item) : answerKey(item)) : undefined;
    const otherKey = key && (choice ? answerKey(item) : choiceKey(item));
    const draft = drafts.get(key);
    const otherDraft = drafts.get(otherKey);
    const target = Object.fromEntries(identity.map((field) => [field, item[field]]));
    const attempt = decisionTarget(item);
    message.textContent = 'Saving decision…';
    try {
      const request = api('inbox/decide', { ...target, decision, data });
      saving.set(attempt, request);
      const response = await request;
      saving.delete(attempt);
      if (!response.accepted) throw new Error('decision_unconfirmed');
      if (draft?.target === attempt && draft.text === data.text && drafts.get(key) === draft) {
        remember(key);
        if (otherDraft && drafts.get(otherKey) === otherDraft) remember(otherKey);
      }
      if (dialog.open && view === 'inbox') await load(response.status === 'approved'
        ? 'Decision saved. The Bot change still needs to be applied in Access settings.' : 'Decision saved.');
    } catch (error) {
      saving.delete(attempt);
      if (key && error.message === 'request_changed') {
        if (drafts.get(key) === draft) remember(key, { target: attempt, text: data.text, stale: true });
        const active = document.activeElement;
        if (dialog.open && view === 'inbox') await load('The request changed.',
          displayed === version && (active === focus?.origin || active === document.body) ? focus : undefined);
        return;
      }
      if (displayed !== version) return;
      const messages = {
        request_changed: 'The request changed. Refresh and review it before deciding again.',
        already_closed: 'This request was already decided or closed. Refresh to see what is still pending.',
        access_denied: 'You do not have permission to make this decision.',
        decision_conflict: 'Flock could not apply this decision. Check the input and refresh.',
      };
      message.textContent = messages[error.message] || 'The decision could not be confirmed. Refresh to check its status.';
    }
  }

  async function load(update = '', focus) {
    const active = document.activeElement;
    if (!focus && items.contains(active) && active.dataset.answerKey) focus = answerFocus(active);
    const displayed = ++version;
    const [title, key, empty, render] = views[view];
    byId('work-view-title').textContent = title;
    history.textContent = view === 'inbox' ? 'Recent decisions' : 'Pending decisions';
    items.replaceChildren();
    const focusWhileLoading = document.activeElement;
    items.setAttribute('aria-busy', 'true');
    message.textContent = 'Loading…';
    const query = new URLSearchParams();
    if (bots.value) query.set('bot', bots.value);
    try {
      const [result, configured] = await Promise.all([api(query.size ? `${view}?${query}` : view), availableBots]);
      if (displayed !== version) return;
      const cards = result[key].map((item) => {
        const card = render(item, focus);
        if (key === 'items') {
          const bot = configured.find(({ id }) => id === item.botId);
          if (bot && sourceAction) card.insertBefore(
            sourceAction(bot, item.conversationEventId, () => dialog.close()),
            card.querySelector('.board-inbox-question, form'));
        }
        return card;
      });
      for (const entry of cards) items.append(entry);
      message.textContent = [update, cards.length ? '' : empty].filter(Boolean).join(' ');
      if (focus && document.activeElement === focusWhileLoading) {
        const answer = Array.from(items.querySelectorAll('textarea')).find((input) => input.dataset.answerKey === focus.key);
        if (answer && !answer.disabled) {
          answer.focus();
          answer.setSelectionRange(...focus.selection);
        }
      }
    } catch {
      if (displayed === version) message.textContent = 'Flock could not load this view. Try refreshing.';
    } finally {
      if (displayed === version) items.removeAttribute('aria-busy');
    }
  }

  for (const button of document.querySelectorAll('[data-work-view]')) {
    button.addEventListener('click', async () => {
      view = button.dataset.workView;
      bots.value = '';
      const allBots = availableBots = api('bots').catch(() => []);
      load();
      dialog.showModal();
      const configured = await allBots;
      if (!dialog.open || allBots !== availableBots) return;
      const selected = bots.value;
      bots.replaceChildren(new Option('All Bots', ''), ...configured.map(({ id, name }) => new Option(name, id)));
      bots.value = selected;
    });
  }
  byId('work-view-close').addEventListener('click', () => dialog.close());
  dialog.addEventListener('close', () => ++version);
  bots.addEventListener('change', () => load());
  byId('work-view-refresh').addEventListener('click', () => load());
  history.addEventListener('click', () => {
    view = view === 'inbox' ? 'inbox/history' : 'inbox';
    load();
  });
}
