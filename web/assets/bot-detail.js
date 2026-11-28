import { showAvatar } from './avatars.js';
import { taskWaitDecision } from './board-inbox.js';
import { createBotRoleCard } from './bot-role.js';
import { createSetupSettings } from './bot-setup.js';
import { createBotWorkspaceCard } from './bot-workspace.js';
import { responseErrors } from './conversation-copy.mjs';
import { node } from './dom.js';
import { createMemoryHistory } from './memory-history.js';
import { updateQuestionChoiceForm } from './question-choices.js';
import { createRecurringRequests } from './recurring-requests.js';
import { placeRows } from './timeline-rows.js';
import {
  assignDetailOrigin,
  assignNotificationDestination,
  assignReopenSettingsOnClose,
  botQuery,
  bots,
  byId,
  conversationActionsTrigger,
  detailNav,
  elements,
  finishingBotEditor,
  fullTime,
  narrow,
  notificationDestination,
  reducedMotion,
  sidebar,
  sidebarShow,
  signatures,
  view,
  workspace,
} from './app-state.js';
import { api, button, changed, linkToRun, selectBot, waitLabel, waitText } from './app-core.js';
import { closeMenu, focusKey } from './sidebar.js';
import { questionActions, readOnlyChoices, showConversationMessage } from './timeline.js';
import { setNotice } from './composer.js';
import { botDeletion, botEditor, botPause, decisions, liveUpdates, operate, refreshBots } from './app.js';

export let detail = null;
const settingsSections = {
  general: ['General', '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="4.5" y="6" width="15" height="12.5" rx="4"/><path d="M12 6V3.5M9.5 12.2v.6M14.5 12.2v.6"/></svg>'],
  memory: ['Memory', '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="5" y="4" width="14" height="16" rx="2"/><path d="M9 9h6m-6 4h6m-6 4h3"/></svg>'],
  recurring: ['Recurring requests', '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="8"/><path d="M12 7.5V12l3 2"/></svg>'],
};
const settingsTabs = Object.keys(settingsSections);
const chevron = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9.5 6.5 5.5 5.5-5.5 5.5"/></svg>';
let detailAvatar = null;
let detailRole = null;
export let detailWorkspace = null;
let detailSetup = null;
let recurringRequests = null;

export function releaseDetailAvatar() {
  detailAvatar?.handle?.destroy();
  detailAvatar = null;
}
function releaseDetailRole() {
  detailRole?.dispose();
  detailRole = null;
}
let detailSectionTimer = 0;
export function clearDetailSectionTransition() {
  clearTimeout(detailSectionTimer);
  detailSectionTimer = 0;
  elements['detail-body'].classList.remove('section-switching');
}
export function setDetail(selection) {
  releaseDetailRole();
  detailWorkspace = null;
  detailSetup = null;
  recurringRequests?.close();
  recurringRequests = selection?.kind === 'settings' ? createRecurringRequests(view.id, api) : null;
  detail = selection;
}
export function closeDetail() {
  setDetail(null);
  clearDetailSectionTransition();
  releaseDetailAvatar();
  if (elements.detail.open) elements.detail.close();
}
export function openDetail(selection) {
  if (!view?.data) return;
  decisions.close({ focus: false });
  clearDetailSectionTransition();
  setDetail(selection.kind === 'settings' ? { section: 'general', ...selection } : selection);
  signatures.delete('detail');
  renderDetail();
  if (elements.detail.open) return;
  assignDetailOrigin(document.activeElement);
  elements.detail.showModal();
  if (!detailNav.hidden) detailNav.querySelector('[aria-selected="true"]')?.focus();
}

export function selectSection(section, focus = false) {
  if (detail?.kind !== 'settings' || detail.section === section) return;
  detail.section = section;
  renderDetail();
  const body = elements['detail-body'];
  body.scrollTop = 0;
  clearDetailSectionTransition();
  if (!reducedMotion.matches) {
    void body.offsetWidth;
    body.classList.add('section-switching');
    detailSectionTimer = setTimeout(() => {
      body.classList.remove('section-switching');
      detailSectionTimer = 0;
    }, 220);
  }
  if (focus) byId(`detail-tab-${section}`)?.focus();
}
function renderDetailNav(sections, current) {
  const body = elements['detail-body'];
  elements.detail.classList.toggle('has-nav', sections.length > 1);
  detailNav.hidden = sections.length < 2;
  if (detailNav.hidden) {
    body.removeAttribute('role');
    body.removeAttribute('aria-labelledby');
    return;
  }
  if (changed('detail-nav', sections)) {
    detailNav.replaceChildren(...sections.map((section) => {
      const [label, icon] = settingsSections[section];
      const tab = button(null, 'detail-tab', () => selectSection(section));
      tab.id = `detail-tab-${section}`;
      tab.dataset.section = section;
      tab.setAttribute('role', 'tab');
      tab.setAttribute('aria-controls', 'detail-body');
      tab.innerHTML = icon;
      tab.append(node('span', null, label));
      return tab;
    }));
  }
  const recurringTab = detailNav.querySelector('[data-section="recurring"]');
  if (recurringTab && recurringRequests && !recurringTab.contains(recurringRequests.count)) {
    recurringTab.querySelector('.detail-tab-count')?.remove();
    const count = node('span', 'detail-tab-count');
    count.append(recurringRequests.count);
    recurringTab.append(count);
  }
  detailNav.setAttribute('aria-orientation', narrow.matches ? 'horizontal' : 'vertical');
  for (const tab of detailNav.children) {
    const selected = tab.dataset.section === current;
    tab.setAttribute('aria-selected', selected);
    tab.tabIndex = selected ? 0 : -1;
  }
  body.setAttribute('role', 'tabpanel');
  body.setAttribute('aria-labelledby', `detail-tab-${current}`);
}

function settingsRow(label, value, click) {
  const row = click ? button(null, 'settings-row', click) : node('div', 'settings-row');
  row.append(node('span', 'settings-row-label', label), node('span', 'settings-row-value', value));
  if (click) row.insertAdjacentHTML('beforeend', chevron);
  return row;
}
function pullRequestSetting(bot) {
  const row = node('label', 'settings-row');
  const label = node('span', 'settings-row-label', 'Fix failing checks and review feedback on its pull requests');
  const toggle = node('input', 'settings-row-switch');
  const feedback = node('span', 'settings-card-feedback error');
  feedback.setAttribute('role', 'status');
  toggle.type = 'checkbox';
  toggle.setAttribute('role', 'switch');
  toggle.checked = bot.fixPullRequests;
  toggle.disabled = !bot.canDelete;
  toggle.addEventListener('change', async () => {
    const fixPullRequests = toggle.checked;
    toggle.disabled = true;
    feedback.textContent = '';
    try {
      const saved = await api('bots/profile', { botId: bot.id, fixPullRequests });
      toggle.checked = saved.fixPullRequests;
      if (view?.id === bot.id) void view.refresh();
    } catch {
      toggle.checked = !fixPullRequests;
      feedback.textContent = 'Couldn’t save this change. Try again.';
    } finally {
      toggle.disabled = false;
    }
  });
  label.append(feedback);
  row.append(label, toggle);
  return row;
}

export function followNotification() {
  if (!notificationDestination || finishingBotEditor || elements['bot-editor'].open) return false;
  const botId = notificationDestination;
  assignNotificationDestination(null);
  selectBot(botId);
  return true;
}

export function openNewBot(origin = elements['new-bot-button']) {
  assignReopenSettingsOnClose(false);
  closeMenu(false);
  closeDetail();
  botEditor.openCreate({ bots, origin });
}

export function headerFocusTarget() {
  return elements['profile-identity'].hidden ? conversationActionsTrigger : elements['profile-avatar-button'];
}

function openEditAppearance(bot) {
  assignReopenSettingsOnClose(true);
  closeDetail();
  botEditor.openEdit(bot, { bots, origin: headerFocusTarget() });
}

export function focusAfterBotDeletion() {
  if (elements.detail.open) {
    (byId('delete-bot-settings') ?? elements['close-detail']).focus();
    return;
  }
  if (narrow.matches && !workspace.classList.contains('nav-open')) {
    elements['open-navigation'].focus();
    return;
  }
  if (!narrow.matches && workspace.classList.contains('sidebar-collapsed')) {
    sidebarShow.focus();
    return;
  }
  if (view) focusKey(`bot:${view.id}`);
  if (!sidebar.contains(document.activeElement)) elements['new-bot-button'].focus();
}

export async function setBotPaused(bot, paused) {
  await api('bots/pause', { botId: bot.id, paused });
  await Promise.all([refreshBots(), view?.id === bot.id && view.refresh()]);
}

const pencilIcon = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 20h9M16.5 3.5a2.12 2.12 0 1 1 3 3L7 19l-4 1 1-4Z"/></svg>';
function generalContent(bot) {
  const canEdit = bot.canDelete === true;
  const hero = node('div', 'settings-hero');
  const face = node('span', 'settings-avatar');
  let avatarHost;
  if (canEdit) {
    avatarHost = button(null, 'settings-avatar-button', () => openEditAppearance(bot));
    avatarHost.setAttribute('aria-label', 'Edit appearance');
    avatarHost.dataset.tooltip = 'Edit appearance';
    avatarHost.dataset.tooltipSide = 'top';
    avatarHost.append(face);
  } else {
    avatarHost = node('span', 'settings-avatar-static');
    avatarHost.append(face);
  }
  detailAvatar = { mount: () => showAvatar(face, bot, { still: false }) };

  const nameWrap = node('div', 'settings-name-wrap');
  const statusNote = node('p', 'settings-status', detail?.status ?? '');
  statusNote.setAttribute('role', 'status');
  statusNote.hidden = !detail?.status;

  function renderNameDisplay() {
    if (!canEdit) {
      nameWrap.replaceChildren(node('span', 'settings-name', bot.name));
      return;
    }
    const nameBtn = button(null, 'settings-name-button', beginInlineRename);
    nameBtn.setAttribute('aria-description', 'Rename Bot');
    nameBtn.append(node('span', 'settings-name', bot.name));
    nameBtn.insertAdjacentHTML('beforeend', pencilIcon);
    nameWrap.replaceChildren(nameBtn);
  }
  function beginInlineRename() {
    const input = node('input', 'settings-name-input');
    input.type = 'text';
    input.maxLength = 32;
    input.value = bot.name;
    input.setAttribute('aria-label', 'Bot name');
    let settled = false;
    async function commit() {
      if (settled) return;
      settled = true;
      const trimmed = input.value.trim();
      if (!trimmed || trimmed === bot.name) {
        renderNameDisplay();
        return;
      }
      input.disabled = true;
      statusNote.classList.remove('error');
      statusNote.textContent = 'Saving name…';
      statusNote.hidden = false;
      try {
        await api('bots/profile', { botId: bot.id, name: trimmed });
        await Promise.all([refreshBots(), view?.id === bot.id && view.refresh()]);
        if (detail?.kind === 'settings') {
          detail = { ...detail, status: 'Name saved.' };
          signatures.delete('detail');
          renderDetail();
        }
      } catch (error) {
        statusNote.classList.add('error');
        statusNote.textContent = error.message === 'invalid_name'
          ? 'Enter a name between 1 and 32 characters.'
          : 'Couldn’t rename this Bot.';
        statusNote.hidden = false;
        renderNameDisplay();
      }
    }
    input.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') {
        event.preventDefault();
        void commit();
      } else if (event.key === 'Escape') {
        event.preventDefault();
        event.stopPropagation();
        settled = true;
        renderNameDisplay();
        nameWrap.querySelector('button')?.focus();
      }
    });
    input.addEventListener('blur', () => { void commit(); });
    nameWrap.replaceChildren(input);
    input.focus();
    input.select();
  }
  renderNameDisplay();
  hero.append(avatarHost, nameWrap, statusNote);
  const rows = node('div', 'settings-card settings-rows');
  rows.append(pullRequestSetting(bot));
  const settings = document.createElement('a');
  settings.className = 'settings-row';
  settings.href = '/settings/' + encodeURIComponent(bot.id);
  settings.append(node('span', 'settings-row-label', 'Access'));
  settings.insertAdjacentHTML('beforeend', chevron);
  rows.append(settings);
  const activity = settingsRow('Status', bot.paused ? 'Paused' : 'Active');
  if (bot.canDelete) {
    const pause = button(bot.paused ? 'Resume…' : 'Pause…', 'button quiet',
      () => botPause.open(bot, () => byId('bot-pause-setting')?.focus({ preventScroll: true })));
    pause.id = 'bot-pause-setting';
    pause.setAttribute('aria-haspopup', 'dialog');
    activity.append(pause);
  }
  rows.append(activity);
  if (bot.canDelete) {
    const remove = button('Delete…', 'settings-row danger', (event) => botDeletion.open(bot, event.currentTarget));
    remove.id = 'delete-bot-settings';
    remove.setAttribute('aria-haspopup', 'dialog');
    rows.append(remove);
  }
  detailSetup ??= createSetupSettings(async (patch) => {
    const saved = await api('bots/profile', { botId: bot.id, ...patch });
    if (view?.id === bot.id) void view.refresh();
    return saved;
  });
  return [hero, workspaceCard(bot), roleCard(bot), detailSetup.element, rows];
}
function workspaceCard(bot) {
  detailWorkspace ??= createBotWorkspaceCard(async (workspace) => {
    const saved = await api('bots/profile', { botId: bot.id, ...workspace });
    if (view?.id === bot.id) void view.refresh();
    return saved;
  }, (environmentId) => api(environmentId ? `settings/projects?${new URLSearchParams({ environmentId })}` : 'settings/projects'),
  () => api(botQuery('settings/models', bot.id)));
  return detailWorkspace.element;
}
function roleCard(bot) {
  detailRole ??= createBotRoleCard(async (soul, originalSoul) => {
    const saved = await api('bots/profile', { botId: bot.id, soul, originalSoul }).catch((error) => {
      if (error.message === 'role_changed' && view?.id === bot.id) void view.refresh();
      throw error;
    });
    void refreshBots();
    if (view?.id === bot.id) void view.refresh();
    return saved.soul;
  });
  return detailRole.element;
}
function settingsContent(bot, section = 'general') {
  elements['detail-title'].textContent = `${bot.name} settings`;
  if (section === 'memory') {
    const context = view;
    return [createMemoryHistory(
      (before) => api(botQuery('memory/history', bot.id, before)),
      (eventId) => {
        closeDetail();
        void showConversationMessage(context, eventId, { preferLoaded: true });
      },
      (run) => linkToRun(run, 'Open Session'),
      bot.name
    )];
  }
  if (section === 'recurring') return [recurringRequests.list, recurringRequests.form];
  return generalContent(bot);
}
function waitContent(context, task) {
  const wait = task.wait;
  const canDecide = !!context.composer;
  elements['detail-title'].textContent = waitLabel(wait.reason);
  const request = node('blockquote', 'wait-request');
  request.append(node('p', 'wait-request-text', task.request));
  if (task.createdAt) request.append(node('p', 'wait-request-meta', `Started ${fullTime.format(task.createdAt * 1000)}`));
  const reason = node('div', 'wait-reason');
  reason.append(node('p', 'wait-text', waitText(wait)));
  const content = [request, reason];
  const actions = node('div', 'wait-actions');
  const primary = node('div', 'wait-primary');
  const perform = (operation, fields) => {
    const current = context.data.waits.find(item => item.id === task.id && item.wait.id === wait.id);
    if (current) void operate(context, current, operation, fields);
  };
  const approveInT3 = canDecide && wait.reason === 't3_input';
  const runLink = linkToRun(wait.run, approveInT3 ? 'Approve in T3' : 'Open in T3');
  if (runLink) {
    runLink.className = approveInT3 ? 'button primary' : 'button quiet';
    primary.append(runLink);
  }
  if (!canDecide) {
    if (wait.choices?.length) reason.append(readOnlyChoices(wait.choices));
    if (runLink) content.push(primary);
    return content;
  }
  if (wait.reason === 'user_question') {
    primary.append(questionActions(context, task, { dialog: true }));
  }
  // Stopping can't be undone, so it asks in place, with the safe choice focused, instead of a browser dialog.
  const stop = button('Stop this request', 'wait-cancel', () => {
    const botName = context.data.bot?.name ?? 'The Bot';
    const confirm = node('div', 'wait-stop-confirm');
    confirm.setAttribute('role', 'group');
    const prompt = node('p', 'wait-stop-text');
    prompt.id = 'wait-stop-text';
    prompt.append(node('strong', null, 'Stop this request?'),
      ` This requests a stop for ${botName}’s work on this request, including any T3 work assigned to it.`);
    confirm.setAttribute('aria-labelledby', prompt.id);
    const keep = button('Keep working', 'button quiet wait-stop-keep', () => {
      actions.replaceChildren(stop, primary);
      stop.focus();
    });
    const confirmStop = button('Stop request', 'button danger', () => perform('cancel'));
    const choices = node('div', 'wait-primary');
    choices.append(keep, confirmStop);
    confirm.append(prompt, choices);
    actions.replaceChildren(confirm);
    keep.focus();
  });
  actions.append(stop, primary);
  const feedback = node('p', 'dialog-feedback');
  feedback.id = 'detail-feedback';
  feedback.setAttribute('role', 'status');
  return [...content, actions, feedback];
}
export function showDetailError(text) {
  const output = byId('detail-feedback');
  if (output) output.textContent = text;
}
async function cancelActiveTask(context, task) {
  const controls = [...elements['detail-body'].querySelectorAll('button, input')];
  for (const control of controls) control.disabled = true;
  showDetailError('');
  try {
    const response = await api('operations', {
      botId: context.id,
      taskId: task.id,
      waitId: task.problem?.id ?? 0,
      targetRevision: task.currentRevision ?? 0,
      targetGeneration: task.generation,
      operation: 'cancel',
    });
    if (response?.accepted !== true) throw new Error('decision_unconfirmed');
    if (view === context && detail?.kind === 'stop-task' && detail.taskId === task.id) closeDetail();
    setNotice(context, 'Stop requested.');
    if (view?.id === context.id) await Promise.all([view.refresh(), !liveUpdates.live && refreshBots()]);
  } catch (error) {
    if (view === context && detail?.kind === 'stop-task' && detail.taskId === task.id) {
      for (const control of controls) control.disabled = false;
      showDetailError(responseErrors[error.message] ?? 'Flock could not confirm the action. Check the conversation.');
    }
  }
}
function stopTaskContent(context, task) {
  elements['detail-title'].textContent = 'Stop this request?';
  const request = node('blockquote', 'wait-request');
  request.append(node('p', 'wait-request-text', task.request));
  if (task.createdAt) request.append(node('p', 'wait-request-meta', `Started ${fullTime.format(task.createdAt * 1000)}`));
  const botName = context.data.bot?.name ?? 'The Bot';
  const explanation = node('p', 'wait-why',
    `This requests a stop for ${botName}’s work on this request, including any T3 work assigned to it.`);
  const keep = button('Keep working', 'button quiet wait-stop-keep', closeDetail);
  keep.autofocus = true;
  const confirmStop = button('Stop request', 'button danger', () => void cancelActiveTask(context, task));
  const choices = node('div', 'wait-primary');
  choices.append(keep, confirmStop);
  const feedback = node('p', 'dialog-feedback');
  feedback.id = 'detail-feedback';
  feedback.setAttribute('role', 'status');
  return [request, explanation, choices, feedback];
}
function labelDetail(label) {
  if (label) {
    elements.detail.setAttribute('aria-label', label);
    elements.detail.removeAttribute('aria-labelledby');
  } else {
    elements.detail.removeAttribute('aria-label');
    elements.detail.setAttribute('aria-labelledby', 'detail-title');
  }
}
function replaceDetail(render) {
  releaseDetailAvatar();
  const content = render();
  if (detailRole && !content.includes(detailRole.element)) releaseDetailRole();
  if (detailWorkspace && !content.includes(detailWorkspace.element)) detailWorkspace = null;
  if (detailSetup && !content.includes(detailSetup.element)) detailSetup = null;
  placeRows(elements['detail-body'], content);
  if (detailAvatar?.mount) detailAvatar.handle = detailAvatar.mount();
}
export function renderDetail() {
  if (!detail || !view) return;
  if (detail.kind === 'settings') {
    const bot = view.data.bot;
    labelDetail(`${bot.name} settings`);
    renderDetailNav(settingsTabs, detail.section);
    const { soul, environmentId, projectId, model, reasoningEffort, setupChangedBy, sessionModel, workspaceStrategy, ...settings } = bot;
    if (changed('detail', detail, settings)) replaceDetail(() => settingsContent(bot, detail.section));
    const canEdit = bot.canDelete === true;
    detailRole?.update(soul, canEdit);
    detailSetup?.update({ setupChangedBy, sessionModel, workspaceStrategy, canDelete: canEdit });
    detailWorkspace?.update({ environmentId, projectId, model, reasoningEffort }, canEdit);
    return;
  }
  labelDetail(null);
  renderDetailNav([], null);
  if (detail.kind === 'stop-task') {
    const task = view.data.activeTasks.find((item) => item.id === detail.taskId);
    if (!task || task.state === 'stopping') {
      if (changed('detail', detail, task?.state ?? null)) {
        elements['detail-title'].textContent = task?.state === 'stopping' ? 'Stopping request' : 'Request closed';
        replaceDetail(() => [node('p', 'wait-why', 'Check the conversation for the latest update.')]);
      }
      return;
    }
    if (changed('detail', detail, task)) replaceDetail(() => stopTaskContent(view, task));
    return;
  }
  const task = view.data.waits.find((item) => item.id === detail.taskId && item.wait.id === detail.waitId);
  if (!task) {
    if (changed('detail', detail, null)) {
      elements['detail-title'].textContent = 'Request closed';
      replaceDetail(() => [node('p', 'wait-why', 'Check the conversation for the latest update.')]);
    }
    return;
  }
  if (changed('detail', detail, task, waitLabel(task.wait.reason), waitText(task.wait)))
    replaceDetail(() => waitContent(view, task));
  const item = taskWaitDecision(view.id, task);
  const busy = decisions.busy(item);
  for (const control of elements['detail-body'].querySelectorAll('button, input')) {
    if (!control.closest('.question-choice-form')) control.disabled = busy;
  }
  const choiceForm = elements['detail-body'].querySelector('.question-choice-form');
  if (choiceForm) {
    const { text, stale } = decisions.choice(item);
    updateQuestionChoiceForm(choiceForm, text, busy || stale);
  }
}
