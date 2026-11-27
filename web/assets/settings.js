import '/tooltip.js';
import { actionLabel, actionLabels } from '/action-labels.mjs';
import {
  browserNotificationsEnabled, listenForBrowserNotifications, notificationPermission, onNotificationPreferenceChange,
  setBrowserNotifications,
} from '/browser-notifications.mjs';
import {
  confirmSetting, loadInstallation, postSettings, renderRemoteMachines, showInstallationUnavailable, statusBadge,
} from '/installation-settings.js';
import { showAvatar } from '/avatars.js';
import { actionButton, element, row, rowLabel, setSettingsStatus, settingsNavigation } from '/settings-ui.js';
import { quietSavedStatus } from '/settings-ui.js';

const $ = (id) => document.getElementById(id);
const setStatus = (text, isError = false) => setSettingsStatus($('status'), text, isError);
const modeInputs = [...document.querySelectorAll('input[name="mode"]')];
let currentMode;
let canManagePolicy = false;
let savingPolicy = false;
const ruleLabels = {never: 'Never', ask: 'Ask first', allow: 'Allow'};

const modeDescriptions = {
  full: 'Bots with access can open pull requests and post to other channels without asking. Never rules still apply.',
  supervised: 'By default, Bots ask before opening pull requests or posting outside the originating conversation.',
};
function describeMode(mode) { $('mode-description').textContent = modeDescriptions[mode] ?? ''; }
function reflectMode(mode) {
  for (const input of modeInputs) input.checked = input.value === mode;
  describeMode(mode);
}
function updatePolicyControls() {
  const disabled = savingPolicy || !canManagePolicy;
  for (const input of modeInputs) input.disabled = disabled;
  for (const control of document.querySelectorAll('#rules button')) control.disabled = disabled;
  for (const control of $('rule-form').elements) control.disabled = disabled;
}

const themeInputs = [...document.querySelectorAll('input[name="theme"]')];
function reflectTheme() {
  const mode = document.documentElement.dataset.theme ?? 'system';
  for (const input of themeInputs) input.checked = input.value === mode;
}
reflectTheme();
document.addEventListener('flock-theme', reflectTheme);
for (const input of themeInputs) {
  input.onchange = () => {
    if (!input.checked) return;
    try { localStorage.setItem('flock-theme', JSON.stringify(input.value)); } catch {}
    document.dispatchEvent(new CustomEvent('flock-theme', { detail: input.value }));
  };
}

const notificationsToggle = $('notifications-toggle');
const notificationsStatus = $('notifications-status');
function reflectNotifications() {
  const note = { denied: 'Blocked in this browser.', unsupported: 'Unavailable in this browser.' }[notificationPermission()];
  notificationsStatus.textContent = note ?? '';
  notificationsStatus.hidden = !note;
  notificationsToggle.hidden = !!note;
  notificationsToggle.textContent = browserNotificationsEnabled() ? 'Turn off' : 'Turn on';
}
notificationsToggle.onclick = async () => {
  notificationsToggle.disabled = true;
  try { await setBrowserNotifications(!browserNotificationsEnabled()); }
  catch { setStatus('Couldn’t change notifications.', true); }
  finally {
    reflectNotifications();
    notificationsToggle.disabled = false;
  }
};
onNotificationPreferenceChange(reflectNotifications);
window.addEventListener('focus', reflectNotifications);
reflectNotifications();
listenForBrowserNotifications();

async function load() {
  const modelAtStart = modelRequest;
  const connectionsAtStart = connectionsRequest;
  const response = await fetch('/api/settings');
  if (!response.ok) throw new Error();
  const data = await response.json();
  const {mode, rules, model} = data;
  canManagePolicy = data.canManagePolicy === true;
  if (modelAtStart === modelRequest && !watchingSignIn) renderModel(model);
  currentMode = mode;
  $('mode-options').classList.toggle('read-only', !canManagePolicy);
  reflectMode(mode);
  $('rules').replaceChildren();
  if (!rules.length) row('rules', canManagePolicy
    ? rowLabel('No rules yet', 'Choose Allow for this Flock on an approval card or add a rule below.')
    : rowLabel('No rules yet', 'An installation administrator can add rules.'));
  for (const {kind, action, target} of rules) {
    const label = document.createElement('span');
    label.className = 'row-label';
    const title = document.createElement('strong');
    title.textContent = actionLabel(action);
    const detail = document.createElement('small');
    detail.textContent = [ruleLabels[kind], target].join(' · ');
    label.append(title, detail);
    const item = row('rules', label);
    if (!canManagePolicy) continue;
    const remove = document.createElement('button');
    remove.textContent = 'Remove';
    remove.className = 'button quiet-danger';
    remove.onclick = () => void submit('rule', {kind, action, target, add: false});
    item.append(remove);
  }
  if (connectionsAtStart === connectionsRequest && !t3Busy()) applyConnections(data);
  $('rule-form').hidden = !canManagePolicy;
  if (canManagePolicy) populateRuleActions(data.personOnlyActions);
  updatePolicyControls();
}

const navigation = settingsNavigation();
quietSavedStatus(document.getElementById('status'));
let initializedSection = false;
function showSection() {
  if (initializedSection) setStatus('');
  initializedSection = true;
  const active = navigation.get(location.hash.slice(1)) ?? navigation.get('appearance');
  const section = active.dataset.section;
  for (const panel of document.querySelectorAll('[data-panel]')) panel.hidden = panel.dataset.panel !== section;
  for (const link of navigation.values()) {
    if (link === active) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  }
  $('crumb').textContent = active.textContent;
  $('settings-scope').textContent = section === 'appearance' ? 'Applies to this browser' : 'Applies to this Flock';
  document.title = `${active.textContent} · Flock settings`;
  if (section === 'bots') void loadBots();
}
addEventListener('hashchange', showSection);
showSection();

function ruleTargetPlaceholder(action = '') {
  if (action.startsWith('repository.')) return 'github.com/acme/app';
  if (action.startsWith('channel.')) return 'slack:workspace-id';
  if (action.startsWith('mcp.')) return 'server-name';
  if (action.startsWith('skill.')) return 'skill-name';
  if (action.startsWith('web.')) return 'example.com';
  if (action.startsWith('environment.') || action.startsWith('session.') || action.startsWith('t3.')) return 'environment-id';
  if (action.startsWith('workspace.')) return '/path/or/glob';
  if (action.startsWith('secret.')) return 'SECRET_NAME';
  return '* or target';
}
function syncRuleTargetPlaceholder() {
  const form = $('rule-form');
  if (form?.elements.target) form.elements.target.placeholder = ruleTargetPlaceholder($('rule-action').value);
}
function populateRuleActions(personOnlyActions) {
  const select = $('rule-action');
  if (!select.options.length) {
    for (const [action, label] of Object.entries(actionLabels)) {
      if (personOnlyActions.includes(action)) continue;
      const option = document.createElement('option');
      option.value = action;
      option.textContent = label;
      select.append(option);
    }
  }
  syncRuleTargetPlaceholder();
}
$('rule-action').onchange = syncRuleTargetPlaceholder;
$('rule-form').onsubmit = async (event) => {
  event.preventDefault();
  const form = event.target;
  const data = Object.fromEntries(new FormData(form));
  if (await submit('rule', { ...data, target: data.target.trim(), add: true })) {
    form.reset();
    syncRuleTargetPlaceholder();
  }
};

function link(label, href, name) {
  const anchor = document.createElement('a');
  anchor.className = 'button';
  anchor.textContent = label;
  anchor.setAttribute('aria-label', name);
  anchor.href = href;
  return anchor;
}
async function loadBots() {
  const list = $('bots');
  try {
    const response = await fetch('/api/bots');
    if (!response.ok) throw new Error();
    const bots = await response.json();
    const rows = document.createDocumentFragment();
    if (!bots.length) row(rows, rowLabel('No Bots yet', 'Create one from the + button in the conversation list.'));
    for (const bot of bots) {
      const label = rowLabel(bot.name, ...(bot.about ? [bot.about] : []));
      const face = element('span', '', 'avatar settings-bot-avatar');
      showAvatar(face, bot);
      const item = row(rows, face);
      item.append(label);
      const actions = element('span', '', 'row-actions');
      actions.append(link('Settings', `/?${new URLSearchParams({bot: bot.id, settings: '1'})}`, `${bot.name} settings`),
        link('Access', `/settings/${encodeURIComponent(bot.id)}`, `Access for ${bot.name}`));
      item.append(actions);
    }
    list.replaceChildren(rows);
  } catch {
    list.replaceChildren();
    row(list, rowLabel('Couldn’t load Bots', 'Reload the page to try again.'));
  }
}

let modelPoll = 0;
let modelRequest = 0;
let watchingSignIn = false;
let signingOut = false;

function pollModel() {
  clearTimeout(modelPoll);
  modelPoll = setTimeout(() => void refreshModel(), 2000);
}
async function refreshModel() {
  const request = ++modelRequest;
  try {
    const response = await fetch('/api/settings');
    if (request !== modelRequest) return;
    if (response.status === 401 || response.status === 403) return renderModel(null);
    if (!response.ok) throw new Error();
    const {model} = await response.json();
    if (request === modelRequest) renderModel(model);
  } catch {
    if (request === modelRequest && watchingSignIn) pollModel();
  }
}

async function signIn(reconsent, control, switchAccount = false) {
  if (watchingSignIn || signingOut) return;
  watchingSignIn = true;
  ++modelRequest;
  control.disabled = true;
  try {
    const response = await postSettings('model-login', {reconsent, switchAccount});
    if (!response.ok && (response.status !== 409 || (await response.json()).error !== 'sign_in_pending')) throw new Error();
  } catch {
    watchingSignIn = false;
    control.disabled = false;
    setStatus('Couldn’t start ChatGPT sign-in.', true);
    return;
  }
  setStatus('');
  renderModel({signingIn: true});
  await refreshModel();
}

async function signOut() {
  if (watchingSignIn || signingOut) return;
  ++modelRequest;
  signingOut = true;
  try {
    const response = await postSettings('model-logout', {});
    if (!response.ok) throw new Error('Couldn’t sign out of ChatGPT. Try again.');
    const {revocationConfirmed} = await response.json();
    await refreshModel();
    setStatus(revocationConfirmed ? 'Signed out of ChatGPT.'
      : 'Signed out on this Flock. ChatGPT did not confirm revoking the previous session.');
  } finally {
    signingOut = false;
  }
}

function modelAccountActions(item, model) {
  const actions = document.createElement('span');
  actions.className = 'row-actions';
  if (model.canSwitch) actionButton(actions, 'Switch account', (event) => void signIn(false, event.currentTarget, true));
  if (model.connected && model.canSignOut) actionButton(actions, 'Sign out', () => confirmSetting('Sign out of ChatGPT?',
    'Bots on this Flock will need a connected account to use ChatGPT.', 'Sign out', signOut));
  if (actions.childElementCount) item.append(actions);
}

function renderModel(model) {
  clearTimeout(modelPoll);
  $('model-section').hidden = !model;
  $('model').replaceChildren();
  const pending = model?.signingIn || (watchingSignIn && model?.unavailable && !model.signInError);
  watchingSignIn = !!pending;
  if (!model) return;
  if (model.demo) {
    row('model', rowLabel('Demo mode', 'Bots reply with a built-in demo instead of ChatGPT.'));
    return;
  }
  if (pending) {
    row('model', rowLabel('Waiting for ChatGPT…', 'Finish signing in with the browser on the computer running Flock.'));
    pollModel();
    return;
  }
  if (model.signInError) row('model', rowLabel('Sign-in didn’t finish', model.signInError)).classList.add('settings-row-error');
  if (model.unavailable) {
    row('model', rowLabel('ChatGPT', 'Flock couldn’t check the connection right now.'));
  } else if (model.connected && !model.needsSignIn) {
    const item = row('model', rowLabel('Signed in', model.account || 'A ChatGPT account is linked to this Flock.'));
    modelAccountActions(item, model);
    if (!model.planUsage) {
      const usage = row('model', rowLabel('Plan usage unavailable', 'Allow plan usage so your Bots can use your ChatGPT plan.'));
      if (model.canSignIn) actionButton(usage, 'Allow plan usage', (event) => void signIn(true, event.currentTarget), true);
    }
  } else {
    const item = row('model', rowLabel('ChatGPT sign-in needed', 'Sign in so your Bots can use your ChatGPT plan.'));
    if (model.canSignIn) actionButton(item, 'Sign in with ChatGPT', (event) => void signIn(false, event.currentTarget), true);
    if (model.canSwitch || (model.connected && model.canSignOut)) modelAccountActions(item, model);
  }
}

let connectionsRequest = 0;
let t3Poll = 0;
let t3Setup;
let savingT3 = false;
let t3ApprovalTab = null;

function t3Busy() {
  return savingT3 || t3Setup?.running === true;
}

function pollConnections() {
  clearTimeout(t3Poll);
  if (t3Setup?.running) t3Poll = setTimeout(() => void refreshConnections(), 2000);
}

function releaseApprovalTab(authorization) {
  const tab = t3ApprovalTab;
  t3ApprovalTab = null;
  if (!tab || tab.closed) return;
  if (authorization) tab.location.replace(authorization);
  else tab.close();
}

function applyConnections(data) {
  t3Setup = data.t3Setup;
  if (t3Setup?.authorizationUrl || !t3Setup?.running) releaseApprovalTab(t3Setup?.authorizationUrl);
  renderConnections(data);
  renderRemoteMachines(data.remoteMachines, Boolean(data.t3), refreshConnections);
  updateConnectionButtons();
  pollConnections();
}

async function refreshConnections() {
  const request = ++connectionsRequest;
  try {
    const response = await fetch('/api/settings');
    if (request !== connectionsRequest) return false;
    if (response.status === 401 || response.status === 403) {
      applyConnections({});
      return false;
    }
    if (!response.ok) throw new Error();
    const data = await response.json();
    if (request !== connectionsRequest) return false;
    applyConnections(data);
    return true;
  } catch {
    if (request === connectionsRequest) pollConnections();
    return false;
  }
}

function renderConnections({t3, t3Setup: setup, canManagePolicy: editable, canDisconnectT3, t3CleanupPending}) {
  const environments = t3?.environments ?? [];
  $('t3-connect-options').hidden = !setup || setup.running;
  $('t3-connect-local').hidden = environments.some(({authorization}) => authorization?.loopbackHttp);
  $('connections').replaceChildren();
  for (const {environmentId, authorization} of environments) {
    const {label, expiresAt, expired} = authorization ?? {};
    const name = label || environmentId;
    const authorized = authorization && !expired;
    const expiry = authorized && expiresAt
      ? `Authorization expires ${new Date(expiresAt * 1000).toLocaleDateString()}`
      : '';
    const item = row('connections', rowLabel(name, expiry));
    const actions = document.createElement('div');
    actions.className = 'connection-actions';
    actions.append(statusBadge(authorized ? 'Authorized' : 'Sign-in needed', authorized ? 'checked' : ''));
    if (setup && authorization)
      actionButton(actions, 'Reconnect', () => void startT3({source: 'saved', environmentId}), expired);
    if (canDisconnectT3) disconnectButton(actions, 'Disconnect', environmentId, name);
    item.append(actions);
  }
  if (!t3 && !setup) {
    row('connections', editable
      ? rowLabel('Demo mode', 'Bots run their work in a built-in demo instead of T3.')
      : rowLabel('Managed by someone else', 'Only the person who connected T3 and Flock admins can see these details.'));
  }
  if (t3CleanupPending) {
    const item = row('connections', rowLabel('Coding agents still list Flock', 'T3 is disconnected, but Flock still appears in Codex or Claude Code on this computer.'));
    disconnectButton(item, 'Remove from coding agents', null);
  }
  if (setup?.running) {
    const authorization = setup.authorizationUrl;
    const item = row('connections', rowLabel(authorization ? 'Authorize Flock in T3' : 'Connecting to T3…',
      authorization ? 'Approve access in T3. This page updates when you’re done.' : 'Waiting for T3 to prepare the approval page.'));
    if (authorization) {
      const open = link('Open T3', authorization, 'Open T3 authorization');
      open.classList.add('primary');
      open.target = '_blank';
      open.rel = 'noopener noreferrer';
      item.append(open);
    }
  } else if (setup?.error) {
    row('connections', rowLabel('Connection didn’t finish', setup.error)).classList.add('settings-row-error');
  }
}

function updateConnectionButtons() {
  const busy = t3Busy();
  for (const button of $('connections').querySelectorAll('button')) button.disabled = busy;
  for (const control of $('t3-form').elements) control.disabled = busy;
  $('t3-connect-local').disabled = busy;
}

async function changeT3(action) {
  if (!canManagePolicy || t3Busy()) return;
  savingT3 = true;
  ++connectionsRequest;
  updateConnectionButtons();
  try { await action(); }
  finally {
    savingT3 = false;
    updateConnectionButtons();
  }
}

function disconnectButton(item, label, environmentId, environmentLabel) {
  const title = environmentId ? `Disconnect ${environmentLabel}?` : 'Remove Flock from coding agents?';
  const description = environmentId
    ? 'Bots using this T3 environment will need it reconnected before starting new work there.'
    : 'Codex and Claude Code on this computer will stop listing Flock.';
  const control = actionButton(item, label, () => confirmSetting(
    title, description, label, () => disconnectT3(environmentId, environmentLabel)));
  control.classList.add('quiet-danger');
}

function disconnectT3(environmentId, label) {
  return changeT3(async () => {
    let saved = false;
    try { saved = (await postSettings('t3-disconnect', {environmentId})).ok; } catch {}
    const refreshed = await refreshConnections();
    setStatus(saved
      ? (refreshed ? (environmentId ? `Disconnected ${label}.` : 'Removed Flock from coding agents.')
                   : 'Saved. Reload to see the current connections.')
      : 'Couldn’t finish disconnecting. Check the current connection status.', !saved);
  });
}

function startT3(target) {
  return changeT3(async () => {
    t3ApprovalTab = window.open('about:blank', '_blank');
    if (t3ApprovalTab) t3ApprovalTab.opener = null;
    try {
      const response = await postSettings('t3-connect', target);
      if (response.ok) {
        if (target.source === 'url') $('t3-form').reset();
      } else {
        releaseApprovalTab();
        if (response.status !== 409 || (await response.json()).error !== 'connect_pending') throw new Error();
      }
      t3Setup = {running: true};
      setStatus('');
      await refreshConnections();
    } catch {
      releaseApprovalTab();
      setStatus(target.source === 'url'
        ? 'Couldn’t start the T3 connection. Check the connection URL.'
        : 'Couldn’t start the T3 connection.', true);
    }
  });
}

$('t3-connect-local').onclick = () => void startT3({source: 'local'});
$('t3-form').onsubmit = (event) => {
  event.preventDefault();
  const {mcpUrl} = Object.fromEntries(new FormData(event.currentTarget));
  void startT3({source: 'url', mcpUrl: mcpUrl.trim()});
};

async function submit(path, data) {
  if (!canManagePolicy || savingPolicy) return false;
  savingPolicy = true;
  updatePolicyControls();
  let saved = false;
  try {
    const response = await postSettings(path, data);
    saved = response.ok;
  } catch {}
  if (saved && path === 'mode') currentMode = data.mode;
  try {
    await load();
    setStatus(saved ? 'Saved.' : 'Couldn’t save this change.', !saved);
  } catch {
    reflectMode(currentMode);
    setStatus(saved ? 'Saved. Reload to see the current settings.' : 'Couldn’t reload settings.', !saved);
  } finally {
    savingPolicy = false;
    updatePolicyControls();
  }
  return saved;
}

for (const input of modeInputs) {
  input.onchange = async () => {
    if (!input.checked || savingPolicy || !canManagePolicy) return;
    describeMode(input.value);
    await submit('mode', {mode: input.value});
  };
}
updatePolicyControls();
load().then(() => {
  if (canManagePolicy) return loadInstallation();
  showInstallationUnavailable();
}).catch(() => { setStatus('Couldn’t load Flock settings.', true); });
