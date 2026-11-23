import { actionLabel, actionLabels } from '/action-labels.mjs';
import {
  actionButton, confirmSetting, emptyLabel, loadInstallation, postSettings, row, showInstallationUnavailable, statusBadge,
} from '/installation-settings.js';

const $ = (id) => document.getElementById(id);
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

// Theme is a per-browser preference shared with the sidebar switch; theme.js applies it.
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
    ? emptyLabel('No rules yet', 'Choose Allow for this Flock on an approval card or add a rule below.')
    : emptyLabel('No rules yet', 'An installation administrator can add rules.'));
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
  if (connectionsAtStart === connectionsRequest && !watchingT3) renderConnections(data);
  $('rule-form').hidden = !canManagePolicy;
  if (canManagePolicy) populateRuleActions(data.personOnlyActions);
  updatePolicyControls();
}

// One section shows at a time, chosen by the URL hash, like a settings window's sidebar.
const sectionNames = { appearance: 'Appearance', permissions: 'Permissions', connections: 'Connections', access: 'Access', system: 'System' };
function showSection() {
  const hash = location.hash.slice(1);
  const section = Object.hasOwn(sectionNames, hash) ? hash : 'appearance';
  for (const panel of document.querySelectorAll('[data-panel]')) panel.hidden = panel.dataset.panel !== section;
  for (const link of document.querySelectorAll('[data-section]')) {
    if (link.dataset.section === section) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  }
  $('crumb').textContent = sectionNames[section];
  $('settings-scope').textContent = section === 'appearance' ? 'Applies to this browser' : 'Applies to this Flock';
  document.title = `${sectionNames[section]} · Flock settings`;
}
addEventListener('hashchange', showSection);
showSection();

function populateRuleActions(personOnlyActions) {
  const select = $('rule-action');
  if (select.options.length) return;
  for (const [action, label] of Object.entries(actionLabels)) {
    if (personOnlyActions.includes(action)) continue;
    const option = document.createElement('option');
    option.value = action;
    option.textContent = label;
    select.append(option);
  }
}
$('rule-form').onsubmit = async (event) => {
  event.preventDefault();
  const form = event.target;
  const data = Object.fromEntries(new FormData(form));
  if (await submit('rule', { ...data, target: data.target.trim(), add: true })) form.reset();
};

function link(label, href, name) {
  const anchor = document.createElement('a');
  anchor.className = 'button';
  anchor.textContent = label;
  anchor.setAttribute('aria-label', name);
  anchor.href = href;
  return anchor;
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
    $('status').textContent = 'Could not start ChatGPT sign-in.';
    return;
  }
  $('status').textContent = '';
  renderModel({signingIn: true});
  await refreshModel();
}

async function signOut() {
  if (watchingSignIn || signingOut) return;
  ++modelRequest;
  signingOut = true;
  try {
    const response = await postSettings('model-logout', {});
    if (!response.ok) throw new Error('Could not sign out of ChatGPT. Try again.');
    const {revocationConfirmed} = await response.json();
    await refreshModel();
    $('status').textContent = revocationConfirmed ? 'Signed out of ChatGPT.'
      : 'Signed out on this Flock. ChatGPT did not confirm revoking the previous session.';
  } finally {
    signingOut = false;
  }
}

function modelAccountActions(item, model) {
  const actions = document.createElement('span');
  actions.className = 'row-actions';
  if (model.canSwitch) actionButton(actions, 'Switch account', (event) => void signIn(false, event.currentTarget, true));
  if (model.canSignOut) actionButton(actions, 'Sign out', () => confirmSetting('Sign out of ChatGPT?',
    'Bots on this Flock will need a connected account to use ChatGPT.', 'Sign out', signOut));
  item.append(actions);
}

function renderModel(model) {
  clearTimeout(modelPoll);
  $('model-section').hidden = !model;
  $('model').replaceChildren();
  const pending = model?.signingIn || (watchingSignIn && model?.unavailable && !model.signInError);
  watchingSignIn = !!pending;
  if (!model) return;
  if (model.demo) {
    row('model', emptyLabel('Demo model', 'Bots use a local demo model on this Flock.'));
    return;
  }
  if (pending) {
    row('model', emptyLabel('Waiting for ChatGPT…', 'Finish signing in in the browser on the machine running Flock.'));
    pollModel();
    return;
  }
  if (model.signInError) row('model', emptyLabel('Sign-in didn’t finish', model.signInError)).classList.add('settings-row-error');
  if (model.unavailable) {
    row('model', emptyLabel('ChatGPT', 'Flock couldn’t check the connection right now.'));
  } else if (model.connected && !model.needsSignIn) {
    const item = row('model', emptyLabel('Signed in', model.account || 'A ChatGPT account is linked to this Flock.'));
    modelAccountActions(item, model);
    if (!model.planUsage) {
      const usage = row('model', emptyLabel('Plan usage unavailable', 'Allow plan usage so your Bots can use your ChatGPT plan.'));
      if (model.canSignIn) actionButton(usage, 'Allow plan usage', (event) => void signIn(true, event.currentTarget), true);
    }
  } else {
    const item = row('model', emptyLabel('ChatGPT sign-in needed', 'Sign in so your Bots can use your ChatGPT plan.'));
    if (model.canSignIn) actionButton(item, 'Sign in with ChatGPT', (event) => void signIn(false, event.currentTarget), true);
    if (model.connected && (model.canSwitch || model.canSignOut)) modelAccountActions(item, model);
  }
}

let connectionsRequest = 0;
let t3Poll = 0;
let watchingT3 = false;
let startingT3 = false;
let disconnectingT3 = false;

function pollConnections() {
  clearTimeout(t3Poll);
  t3Poll = setTimeout(() => void refreshConnections(), 2000);
}

async function refreshConnections() {
  const request = ++connectionsRequest;
  try {
    const response = await fetch('/api/settings');
    if (request !== connectionsRequest) return;
    if (response.status === 401 || response.status === 403) return renderConnections({});
    if (!response.ok) throw new Error();
    const data = await response.json();
    if (request !== connectionsRequest) return false;
    renderConnections(data);
    return true;
  } catch {
    if (request === connectionsRequest && watchingT3) pollConnections();
  }
}

function renderConnections({t3, t3Setup, canManagePolicy: editable, canDisconnectT3, t3CleanupPending}) {
  clearTimeout(t3Poll);
  $('t3-form').hidden = !t3Setup || t3Setup.running;
  watchingT3 = t3Setup?.running === true;
  $('connections').replaceChildren();
  if (t3) {
    for (const environment of t3.environments) {
      const item = row('connections', emptyLabel(environment, 'Sessions in this environment can ask your Bots.'));
      const actions = document.createElement('div');
      actions.className = 'connection-actions';
      actions.append(statusBadge('Authorized', 'checked'));
      if (canDisconnectT3) disconnectButton(actions, 'Disconnect', environment);
      item.append(actions);
    }
  } else if (editable && t3Setup) {
    row('connections', emptyLabel('Not connected', 'In T3 Code, open Settings → Environments and choose Copy MCP URL.'));
  } else if (editable) {
    row('connections', emptyLabel('Not connected', 'T3 setup is unavailable in this Flock instance.'));
  } else {
    row('connections', emptyLabel('No connection details available', 'The person who connected T3 and installation administrators can see its details.'));
  }
  if (t3CleanupPending) {
    const item = row('connections', emptyLabel('Provider registration remains', 'T3 access is revoked. Retry removing the Flock registration from local coding agents.'));
    disconnectButton(item, 'Retry cleanup', null);
  }
  if (watchingT3) {
    const authorization = t3Setup.authorizationUrl;
    const item = row('connections', emptyLabel(authorization ? 'Authorize Flock in T3' : 'Connecting to T3…',
      authorization ? 'Approve read and operate access in T3. This page updates when you’re done.' : 'Starting T3 authorization.'));
    if (authorization) {
      const open = link('Open T3', authorization, 'Open T3 authorization');
      open.classList.add('primary');
      open.target = '_blank';
      open.rel = 'noopener noreferrer';
      item.append(open);
    }
    pollConnections();
  } else if (t3Setup?.error) {
    row('connections', emptyLabel('Connection didn’t finish', t3Setup.error)).classList.add('settings-row-error');
  }
}

function updateConnectionButtons() {
  for (const button of $('connections').querySelectorAll('button[data-t3-disconnect]')) {
    button.disabled = startingT3 || watchingT3 || disconnectingT3;
  }
  for (const control of $('t3-form').elements) control.disabled = disconnectingT3 || startingT3;
}

function disconnectButton(item, label, environment) {
  const control = actionButton(item, label, () => void disconnectT3(environment));
  control.classList.add('quiet-danger');
  control.dataset.t3Disconnect = '';
  control.disabled = startingT3 || watchingT3 || disconnectingT3;
}

async function disconnectT3(environment) {
  if (startingT3 || watchingT3 || disconnectingT3) return;
  disconnectingT3 = true;
  ++connectionsRequest;
  updateConnectionButtons();
  let saved = false;
  try {
    const response = await postSettings('t3-disconnect', {environmentId: environment});
    saved = response.ok;
  } catch {}
  const refreshed = await refreshConnections();
  $('status').textContent = saved
    ? (refreshed ? (environment ? `Disconnected ${environment}.` : 'Provider registration removed.')
                  : 'Saved. Reload to see the current connections.')
    : 'Could not finish disconnecting. Check the current connection status.';
  disconnectingT3 = false;
  updateConnectionButtons();
}

$('t3-form').onsubmit = async (event) => {
  event.preventDefault();
  if (!canManagePolicy || startingT3 || watchingT3 || disconnectingT3) return;
  const form = event.currentTarget;
  const {mcpUrl} = Object.fromEntries(new FormData(form));
  startingT3 = true;
  ++connectionsRequest;
  updateConnectionButtons();
  try {
    const response = await postSettings('t3-connect', {mcpUrl: mcpUrl.trim()});
    if (response.ok) form.reset();
    else if (response.status !== 409 || (await response.json()).error !== 'connect_pending') throw new Error();
    watchingT3 = true;
    $('status').textContent = '';
    await refreshConnections();
  } catch {
    $('status').textContent = 'Could not start the T3 connection. Check the connection URL.';
  } finally {
    startingT3 = false;
    updateConnectionButtons();
  }
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
    $('status').textContent = saved ? 'Saved.' : 'Could not save this change.';
  } catch {
    reflectMode(currentMode);
    $('status').textContent = saved ? 'Saved. Reload to see the current settings.' : 'Could not reload settings.';
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
}).catch(() => { $('status').textContent = 'Could not load Flock settings.'; });
