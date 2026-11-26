const setupLabels = {
  name: 'Name', avatar: 'Appearance', soul: 'Role', model: 'Model', reasoningEffort: 'Reasoning effort',
  environmentId: 'T3 environment', projectId: 'T3 project', sessionModel: 'Session model',
  workspaceStrategy: 'Session workspace', fixPullRequests: 'PR follow-ups',
};
const workspaceNames = { root: 'Project root', worktree: 'New worktree', existing_worktree: 'Existing worktree' };

function element(tag, text = '', className = '') {
  const result = document.createElement(tag);
  result.textContent = text;
  if (className) result.className = className;
  return result;
}

export function orderSetupNotices(events) {
  const replies = new Map(events.filter(event => event.kind === 'conversation_reply')
    .map(event => [event.data.replyToMessageId, event.id]));
  const anchored = new Map();
  const visible = events.filter(event => {
    if (event.kind !== 'bot_setup_updated') return true;
    const anchor = replies.get(event.data.sourceMessageId);
    if (!anchor || anchor < event.id) return true;
    if (!anchored.has(anchor)) anchored.set(anchor, []);
    anchored.get(anchor).push(event);
    return false;
  });
  return visible.flatMap(event => [event, ...(anchored.get(event.id) ?? [])]);
}

function roleDiff(before, after) {
  const lines = text => text.match(/[^\n]*\n|[^\n]+$/g) ?? [];
  const oldLines = lines(before);
  const newLines = lines(after);
  let start = 0, tail = 0;
  while (start < Math.min(oldLines.length, newLines.length) && oldLines[start] === newLines[start]) ++start;
  while (tail < Math.min(oldLines.length, newLines.length) - start &&
    oldLines.at(-1 - tail) === newLines.at(-1 - tail)) ++tail;
  const removed = oldLines.slice(start, oldLines.length - tail);
  const added = newLines.slice(start, newLines.length - tail);
  const counts = [removed.length && `−${removed.length}`, added.length && `+${added.length}`].filter(Boolean).join(' ');
  const diff = element('details', '', 'setup-role-diff');
  diff.append(element('summary', `Role: ${counts} ${removed.length + added.length === 1 ? 'line' : 'lines'}`));
  const content = element('pre');
  for (const [sign, items, tag] of [['−', removed, 'del'], ['+', added, 'ins']])
    for (const line of items) content.append(element(tag, `${sign} ${line.endsWith('\n') ? line : line + '\n'}`));
  diff.append(content);
  return diff;
}

function settingValue(field, value) {
  if (value == null || value === '') return 'Default';
  if (field === 'avatar') return value.example ?? value.icon ?? 'Uploaded image';
  if (field === 'workspaceStrategy') return [workspaceNames[value.type], value.baseRef, value.worktreePath].filter(Boolean).join(' · ');
  if (field === 'sessionModel') return `${value.model} (${value.providerInstanceId})`;
  if (field === 'fixPullRequests') return value ? 'On' : 'Off';
  return value;
}

export function setupNotice(event, canUndo, undoChange) {
  const { before, after, undone } = event.data;
  const note = element('article', '', 'system-note memory-notice');
  note.id = `event-${event.id}`;
  note.tabIndex = -1;
  note.append(element('strong', undone ? 'Update undone' : 'Updated'));
  for (const field of Object.keys(setupLabels)) {
    if (!Object.hasOwn(after, field)) continue;
    if (field === 'reasoningEffort' && Object.hasOwn(after, 'model')) continue;
    note.append(' · ');
    if (field === 'soul') note.append(roleDiff(before.soul, after.soul));
    else if (field === 'model') {
      const effort = after.reasoningEffort ? ` (${after.reasoningEffort})` : '';
      note.append(`Model: ${after.model.model || 'Default'}${effort}`);
    } else note.append(`${setupLabels[field]}: ${settingValue(field, after[field])}`);
  }
  if (canUndo && !undone) {
    const undo = element('button', 'Undo', 'text-button');
    undo.type = 'button';
    undo.dataset.timelineAction = 'undo-setup';
    const status = element('span', '', 'memory-notice-status');
    status.setAttribute('role', 'status');
    undo.addEventListener('click', async () => {
      if (undo.disabled) return;
      undo.disabled = true;
      status.textContent = 'Undoing…';
      try {
        await undoChange();
      } catch (error) {
        status.textContent = error.status === 409 ? 'Bot settings have changed since this update.' : 'Could not undo. Try again.';
        undo.disabled = error.status === 409;
      }
    });
    note.append(' · ', undo, status);
  }
  return note;
}

export function createSetupSettings(save) {
  const panel = element('details', '', 'settings-card setup-details');
  panel.append(element('summary', 'Setup details', 'settings-card-label'));
  const facts = element('dl', '', 'settings-workspace-facts');
  const form = element('form', '', 'settings-workspace-form');
  const edited = new Set();
  const input = (label, type = 'text') => {
    const row = element('label', label, 'settings-workspace-field');
    const control = element('input');
    control.type = type;
    row.append(control);
    form.append(row);
    return control;
  };
  const provider = input('Session provider ID');
  const model = input('Session model ID');
  const select = element('select');
  for (const [value, label] of [['', 'Default'], ...Object.entries(workspaceNames)]) select.add(new Option(label, value));
  const workspace = element('label', 'Session workspace', 'settings-workspace-field');
  workspace.append(select);
  form.append(workspace);
  const base = input('Base ref');
  const branch = input('Branch');
  const path = input('Worktree path');
  const origin = input('Start from origin', 'checkbox');
  const submit = element('button', 'Save Session settings', 'button primary');
  const status = element('p', '', 'settings-workspace-hint');
  status.setAttribute('role', 'status');
  form.append(submit, status);
  panel.append(facts, form);
  let saving = false;
  const updateFields = () => {
    for (const field of [base, branch, origin]) field.parentElement.hidden = select.value !== 'worktree';
    path.parentElement.hidden = select.value !== 'existing_worktree';
    submit.disabled = saving || !edited.size;
    for (const control of [provider, model, select, base, branch, path, origin]) control.disabled = saving;
  };
  function update(bot) {
    const { setupChangedBy = {}, sessionModel, workspaceStrategy } = bot;
    facts.replaceChildren();
    for (const [field, label] of Object.entries(setupLabels)) {
      const authored = setupChangedBy[field];
      const session = field === 'sessionModel' || field === 'workspaceStrategy';
      if (!authored && !session) continue;
      const description = authored === 'bot' ? 'Last changed by Bot' : authored === 'person' ? 'Last changed by person' : '';
      facts.append(element('dt', label),
        element('dd', [session && settingValue(field, bot[field]), description].filter(Boolean).join(' · ')));
    }
    form.hidden = !bot.canDelete;
    if (!edited.has('sessionModel')) {
      provider.value = sessionModel?.providerInstanceId ?? '';
      model.value = sessionModel?.model ?? '';
    }
    if (!edited.has('workspaceStrategy')) {
      select.value = workspaceStrategy?.type ?? '';
      base.value = workspaceStrategy?.baseRef ?? '';
      branch.value = workspaceStrategy?.branch ?? '';
      path.value = workspaceStrategy?.worktreePath ?? '';
      origin.checked = workspaceStrategy?.startFromOrigin ?? false;
    }
    updateFields();
  }
  for (const [field, controls] of [['sessionModel', [provider, model]], ['workspaceStrategy', [select, base, branch, path, origin]]]) {
    for (const control of controls) {
      control.addEventListener('input', () => {
        edited.add(field);
        status.textContent = '';
        updateFields();
      });
    }
  }
  form.addEventListener('submit', async event => {
    event.preventDefault();
    if (saving || !edited.size) return;
    const patch = {};
    if (edited.has('sessionModel')) {
      const providerInstanceId = provider.value.trim();
      const modelId = model.value.trim();
      patch.sessionModel = providerInstanceId || modelId ? { providerInstanceId, model: modelId } : null;
    }
    if (edited.has('workspaceStrategy')) {
      const strategy = select.value ? { type: select.value } : null;
      if (select.value === 'worktree') {
        strategy.baseRef = base.value.trim();
        if (branch.value.trim()) strategy.branch = branch.value.trim();
        strategy.startFromOrigin = origin.checked;
      }
      if (select.value === 'existing_worktree') strategy.worktreePath = path.value.trim();
      patch.workspaceStrategy = strategy;
    }
    saving = true;
    updateFields();
    try {
      const saved = await save(patch);
      edited.clear();
      update({ ...saved, canDelete: true });
      status.textContent = 'Session settings saved.';
    } catch {
      status.textContent = 'Check the Session provider, model and workspace settings.';
    } finally {
      saving = false;
      updateFields();
    }
  });
  return { element: panel, update };
}
