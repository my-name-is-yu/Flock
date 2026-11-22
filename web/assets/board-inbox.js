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
  pr_reconciliation: [['acknowledge', 'Acknowledge']],
  pr_review: [['approve', 'Approve review'], ['request_changes', 'Request changes']],
};
const identity = ['botId', 'itemKind', 'itemId', 'waitId', 'taskId', 'targetRevision', 'targetGeneration'];

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
function decisionCard(item, submit) {
  const article = card(item.requestTitle, `${item.botName} · ${labels[item.itemKind]}`);
  if (item.request && item.request !== item.requestTitle) article.append(element('p', item.request));
  article.append(element('p', item.prompt));
  if (item.prUrl) article.append(prLink(item.prUrl, item.prTitle));
  if (!item.canDecide) {
    article.append(element('p', 'View only', 'board-inbox-meta'));
    return article;
  }
  const form = element('form', '', 'board-inbox-form');
  const inputs = {};
  if (item.itemKind === 'user_question') {
    inputs.text = element('textarea');
    inputs.text.rows = 2;
    inputs.text.maxLength = 8000;
    inputs.text.required = true;
    form.append(labeled('Answer', inputs.text));
    if (item.choices?.length) {
      const choices = element('div', '', 'board-inbox-actions');
      for (const choice of item.choices) {
        const button = element('button', choice);
        button.type = 'button';
        button.addEventListener('click', () => { inputs.text.value = choice; });
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
  const buttons = element('div', '', 'board-inbox-actions');
  for (const [decision, title] of actions[item.itemKind]) {
    const button = element('button', title);
    button.value = decision;
    button.type = 'submit';
    buttons.append(button);
  }
  form.append(buttons);
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (!event.submitter) return;
    const data = Object.fromEntries(Object.entries(inputs).map(([field, input]) => [field, input.value]));
    if (data.deadline) data.deadline = Math.floor(new Date(data.deadline).getTime() / 1000);
    const fields = [...form.elements];
    for (const input of fields) input.disabled = true;
    try { await submit(item, event.submitter.value, data); }
    finally {
      for (const input of fields) input.disabled = false;
    }
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
  const views = {
    board: ['Task board', 'tasks', 'No matching tasks.', taskCard],
    inbox: ['Decision inbox', 'items', 'No pending decisions.', (item) => decisionCard(item, submit)],
    'inbox/history': ['Recent decisions', 'decisions', 'No recorded decisions.', (entry) => card(labels[entry.itemKind],
      `${entry.botName} · ${labels[entry.decision]} by ${entry.decidedBy} · ${time(entry.decidedAt)}`)],
  };
  let view;
  let version = 0;

  async function submit(item, decision, data) {
    const displayed = version;
    message.textContent = 'Saving decision…';
    try {
      const target = Object.fromEntries(identity.map((field) => [field, item[field]]));
      const response = await api('inbox/decide', { ...target, decision, data });
      if (displayed === version) await load(response.status === 'approved'
        ? 'Decision saved. The Bot change still needs to be applied in Access settings.' : 'Decision saved.');
    } catch (error) {
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

  async function load(update = '') {
    const displayed = ++version;
    const [title, key, empty, render] = views[view];
    byId('work-view-title').textContent = title;
    byId('work-view-status-label').hidden = view !== 'board';
    history.hidden = view === 'board';
    history.textContent = view === 'inbox' ? 'Recent decisions' : 'Pending decisions';
    items.replaceChildren();
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
