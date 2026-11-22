const labels = {
  user_question: 'Question', extra_run: 'Extra run', deadline: 'Deadline extension',
  model_budget: 'Extra model call', conversation_model_budget: 'Extra conversation model call',
  unknown_delivery: 'Delivery confirmation', grant_request: 'Access request',
  protected_input: 'Protected input', bot_change: 'Bot change', pr_review: 'Pull request review',
  pr_reconciliation: 'Pull request reconciliation',
  budget: 'Run allowance', model_failure: 'Model failure', t3_input: 'Action in T3',
  auth: 'T3 sign-in', settings: 'T3 settings',
  blocked: 'Blocked', waiting: 'Waiting', queued: 'Queued', deciding: 'Evaluating',
  sending: 'Sending', running: 'Working', replanning: 'Replanning', peer_waiting: 'Waiting for another Bot',
  cancelling: 'Cancelling', cancelled: 'Cancelled', completed: 'Complete',
  answer: 'Answered', approve: 'Approved', deny: 'Denied', supply: 'Supplied',
  extend: 'Extended', reconcile: 'Reconciliation requested', request_changes: 'Changes requested',
  link_pr: 'Pull request linked', reject_pr: 'Operation rejected',
};
const actions = {
  user_question: [['answer', 'Send answer']],
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
function taskCard(task) {
  const article = card(task.requestTitle, `${task.botName} · ${labels[task.status] || task.status}`);
  const details = element('dl');
  const field = (label, value) => {
    if (value) details.append(element('dt', label), element('dd', value));
  };
  if (task.description !== task.requestTitle) field('Request', task.description);
  field('Done when', task.doneWhen);
  field('Deadline', task.deadlineAt && time(task.deadlineAt));
  field('Blocker', task.blockerReason);
  if (task.wait && task.wait.text !== task.blockerReason)
    field('Current wait', task.wait.text || labels[task.wait.reason] || task.wait.reason);
  if (details.children.length) article.append(details);
  for (const pr of task.prRefs) article.append(prLink(pr.url, pr.title));
  return article;
}
function decisionCard(item, submit, drafts, remember, saving) {
  const article = card(item.requestTitle, `${item.botName} · ${labels[item.itemKind]}`);
  if (item.request && item.request !== item.requestTitle) article.append(element('p', item.request));
  article.append(element('p', item.prompt));
  if (item.prUrl) article.append(prLink(item.prUrl, item.prTitle));
  for (const pr of item.candidates || []) article.append(prLink(pr.url, `Pull request #${pr.prNumber}`));
  if (!item.canDecide) {
    article.append(element('p', 'View only', 'board-inbox-meta'));
    return article;
  }
  const form = element('form', '', 'board-inbox-form');
  const inputs = {};
  const buttons = element('div', '', 'board-inbox-actions');
  let review;
  if (item.itemKind === 'user_question') {
    const key = answerKey(item);
    const target = decisionTarget(item);
    const draft = drafts.get(key);
    let draftTarget = draft?.target ?? target;
    let stale = draft?.stale;
    const changed = draftTarget !== target;
    inputs.text = element('textarea');
    inputs.text.dataset.answerKey = key;
    inputs.text.rows = 2;
    inputs.text.maxLength = 8000;
    inputs.text.required = true;
    inputs.text.value = draft?.text ?? '';
    const saveAnswer = () => remember(key, { target: draftTarget, text: inputs.text.value, stale });
    const useCurrentQuestion = () => {
      draftTarget = target;
      stale = false;
      saveAnswer();
      review?.remove();
      review = undefined;
      setBusy(false);
    };
    inputs.text.addEventListener('input', () => {
      if (draftTarget !== target && inputs.text.value !== draft?.text) useCurrentQuestion();
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
          inputs.text.focus();
        });
        review.append(useDraft);
      }
      form.append(review);
    }
    form.append(labeled('Answer', inputs.text));
    if (item.choices?.length) {
      const choices = element('div', '', 'board-inbox-actions');
      for (const choice of item.choices) {
        const button = element('button', choice);
        button.type = 'button';
        button.addEventListener('click', () => {
          inputs.text.value = choice;
          if (draftTarget !== target) useCurrentQuestion();
          else saveAnswer();
        });
        choices.append(button);
      }
      form.append(choices);
    }
  } else if (item.itemKind === 'deadline') {
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
    for (const input of form.elements) input.disabled = busy || (input.type === 'submit' && Boolean(review));
  }
  setBusy(Boolean(saving));
  saving?.then(() => setBusy(false), () => setBusy(false));
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (!event.submitter || event.submitter.disabled) return;
    const data = Object.fromEntries(Object.entries(inputs).map(([field, input]) => [field, input.value]));
    if (event.submitter.dataset.prNumber) data.prNumber = Number(event.submitter.dataset.prNumber);
    if (data.deadline) data.deadline = Math.floor(new Date(data.deadline).getTime() / 1000);
    const active = document.activeElement;
    const focus = inputs.text && form.contains(active) ? { ...answerFocus(inputs.text), origin: active } : undefined;
    setBusy(true);
    try { await submit(item, event.submitter.value, data, focus); }
    finally { setBusy(false); }
  });
  article.append(form);
  return article;
}

export function mountBoardInbox(api) {
  const byId = (id) => document.getElementById(id);
  const dialog = byId('board-inbox');
  const bots = byId('work-view-bot');
  const status = byId('work-view-status');
  const message = byId('work-view-message');
  const items = byId('work-view-items');
  const history = byId('work-view-history');
  let drafts = new Map();
  try { drafts = new Map(JSON.parse(sessionStorage.getItem(draftStorage))); } catch {}
  const saving = new Map();
  const views = {
    board: ['Task board', 'tasks', 'No matching tasks.', taskCard],
    inbox: ['Decision inbox', 'items', 'No pending decisions.', (item) =>
      decisionCard(item, submit, drafts, remember, saving.get(decisionTarget(item)))],
    'inbox/history': ['Recent decisions', 'decisions', 'No recorded decisions.', (entry) => card(labels[entry.itemKind],
      `${entry.botName} · ${labels[entry.decision]} by ${entry.decidedBy} · ${time(entry.decidedAt)}`)],
  };
  let view;
  let version = 0;

  function remember(key, draft) {
    if (draft?.text || draft?.stale) drafts.set(key, draft);
    else drafts.delete(key);
    try { sessionStorage.setItem(draftStorage, JSON.stringify([...drafts])); } catch {}
  }

  async function submit(item, decision, data, focus) {
    const displayed = version;
    const key = decision === 'answer' ? answerKey(item) : undefined;
    const draft = drafts.get(key);
    const target = Object.fromEntries(identity.map((field) => [field, item[field]]));
    const attempt = decisionTarget(item);
    message.textContent = 'Saving decision…';
    try {
      const request = api('inbox/decide', { ...target, decision, data });
      saving.set(attempt, request);
      const response = await request;
      saving.delete(attempt);
      if (!response.accepted) throw new Error('decision_unconfirmed');
      if (draft?.target === attempt && draft.text === data.text && drafts.get(key) === draft) remember(key);
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
    byId('work-view-status-label').hidden = view !== 'board';
    history.hidden = view === 'board';
    history.textContent = view === 'inbox' ? 'Recent decisions' : 'Pending decisions';
    items.replaceChildren();
    const focusWhileLoading = document.activeElement;
    items.setAttribute('aria-busy', 'true');
    message.textContent = 'Loading…';
    const query = new URLSearchParams();
    if (bots.value) query.set('bot', bots.value);
    if (view === 'board' && status.value) query.set('status', status.value);
    try {
      const result = await api(query.size ? `${view}?${query}` : view);
      if (displayed !== version) return;
      const cards = result[key].map(render);
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
      const allBots = api('bots').catch(() => []);
      load();
      dialog.showModal();
      const configured = await allBots;
      if (!dialog.open) return;
      const selected = bots.value;
      bots.replaceChildren(new Option('All Bots', ''), ...configured.map(({ id, name }) => new Option(name, id)));
      bots.value = selected;
    });
  }
  byId('work-view-close').addEventListener('click', () => dialog.close());
  dialog.addEventListener('close', () => ++version);
  for (const input of [bots, status]) input.addEventListener('change', () => load());
  byId('work-view-refresh').addEventListener('click', () => load());
  history.addEventListener('click', () => {
    view = view === 'inbox' ? 'inbox/history' : 'inbox';
    load();
  });
}
