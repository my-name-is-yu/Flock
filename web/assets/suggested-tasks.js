import { placeRows } from '/timeline-rows.js';

function element(tag, className, text) {
  const el = document.createElement(tag);
  if (className) el.className = className;
  if (text != null) el.textContent = text;
  return el;
}

function action(label, className, activate) {
  const control = element('button', className, label);
  control.type = 'button';
  if (activate) control.addEventListener('click', activate);
  return control;
}

function oneLine(text, className) {
  const line = element('p', className, text);
  line.title = text;
  return line;
}

export function createSuggestedTaskRow(item, { canAct, start, dismiss, openTask }) {
  const row = element('li', 'task-suggestion');
  const title = item.request.trim().split('\n', 1)[0];
  const details = element('div', 'task-suggestion-details');
  details.append(oneLine(title, 'task-suggestion-title'), oneLine(item.destination), oneLine(item.reason));
  row.append(details);

  const controls = element('div', 'task-suggestion-actions');
  row.append(controls);
  if (item.status === 'started') {
    const link = element('a', 'task-suggestion-link link', 'Started');
    link.href = `#event-${item.taskEventId}`;
    link.setAttribute('aria-label', `View started task: ${title}`);
    link.addEventListener('click', event => {
      if (event.button || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
      event.preventDefault();
      openTask(item.taskEventId);
    });
    controls.append(link);
  } else if (item.status === 'dismissed') {
    controls.append(element('span', 'task-suggestion-status', 'Dismissed'));
  }
  if (!canAct || item.status !== 'suggested') return { row };

  const original = { id: item.id };
  const form = element('form', 'task-suggestion-form');
  form.hidden = true;
  const request = element('textarea', 'task-suggestion-request');
  request.rows = 3;
  request.required = true;
  request.setAttribute('aria-label', `Edit request: ${title}`);
  const draft = () => form.hidden ? original : { id: item.id, request: request.value.trim() };
  const startButton = action('Start', 'button', () => start([original]));
  const edit = action('Edit', 'text-button', () => {
    request.value = item.request;
    controls.hidden = true;
    form.hidden = false;
    request.focus();
  });
  const remove = action('Dismiss', 'text-button', () => dismiss(item.id));
  for (const control of [startButton, edit, remove]) {
    control.setAttribute('aria-label', `${control.textContent}: ${title}`);
  }
  controls.append(edit, remove, startButton);
  const cancelEdit = () => {
    form.hidden = true;
    controls.hidden = false;
    edit.focus();
  };
  const cancel = action('Cancel', 'text-button', cancelEdit);
  const submit = action('Start', 'button');
  submit.type = 'submit';
  const editorActions = element('div', 'task-suggestion-actions');
  editorActions.append(cancel, submit);
  form.append(request, editorActions);
  form.addEventListener('submit', event => {
    event.preventDefault();
    start([draft()]);
  });
  form.addEventListener('keydown', event => {
    if (event.key !== 'Escape') return;
    event.stopPropagation();
    cancelEdit();
  });
  row.append(form);
  return { row, draft };
}

const updates = new WeakMap();

export function createSuggestedTasksCard({ canAct, start, dismiss, openTask }) {
  const card = element('article', 'suggested-tasks-card');
  card.tabIndex = -1;
  card.setAttribute('aria-label', 'Suggested next');
  const list = element('ul', 'task-suggestion-list');
  const footer = element('footer', 'task-suggestion-footer');
  const status = element('p', 'task-suggestion-feedback');
  status.setAttribute('role', 'status');
  let rows = new Map();
  let busy = false;

  async function perform(mutate, errorText) {
    if (busy) return;
    busy = true;
    card.setAttribute('aria-busy', 'true');
    status.textContent = '';
    try {
      await mutate();
    } catch {
      status.textContent = errorText;
    } finally {
      busy = false;
      card.removeAttribute('aria-busy');
    }
  }

  const actions = {
    canAct, openTask,
    start(items) {
      if (items.some(item => item.request === '')) {
        status.textContent = 'Enter a request before starting.';
        return;
      }
      return perform(() => start(items), 'Could not start the work. Try again.');
    },
    dismiss(id) {
      return perform(() => dismiss(id), 'Could not dismiss the suggestion. Try again.');
    },
  };
  const startAll = action('Start all', 'button', () => actions.start(
    [...rows.values()].flatMap(({ draft }) => draft ? [draft()] : [])));
  footer.append(startAll);
  card.append(element('h3', 'task-suggestion-heading', 'Suggested next'), list, footer, status);
  updates.set(card, items => {
    const active = document.activeElement;
    const wasFocused = list.contains(active);
    rows = new Map(items.map(item => {
      const version = JSON.stringify(item);
      const previous = rows.get(item.id);
      return [item.id, previous?.version === version ? previous : { ...createSuggestedTaskRow(item, actions), version }];
    }));
    placeRows(list, [...rows.values()].map(({ row }) => row));
    footer.hidden = ![...rows.values()].some(({ draft }) => draft);
    if ((wasFocused && !list.contains(active)) || (active === startAll && footer.hidden))
      card.focus({ preventScroll: true });
  });
  return card;
}

export function updateSuggestedTasksCard(card, items) {
  updates.get(card)(items);
}
