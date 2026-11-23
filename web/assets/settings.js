import { actionLabel, actionLabels } from '/action-labels.mjs';

const $ = (id) => document.getElementById(id);
const modeInputs = [...document.querySelectorAll('input[name="mode"]')];
let currentMode;
const ruleLabels = {never: 'Never', ask: 'Ask first', allow: 'Allow'};

function emptyLabel(title, ...detail) {
  const label = document.createElement('span');
  label.className = 'row-label';
  const heading = document.createElement('strong');
  heading.textContent = title;
  const sub = document.createElement('small');
  sub.append(...detail);
  label.append(heading, sub);
  return label;
}

function command(text) {
  const code = document.createElement('code');
  code.textContent = text;
  return code;
}

function row(list, content) {
  const item = document.createElement('div');
  item.className = 'settings-row';
  item.append(content);
  $(list).append(item);
  return item;
}

const modeDescriptions = {
  full: 'Bots use the access they’re given without asking. Never rules still apply.',
  supervised: 'Bots ask before actions with outside effects, such as opening pull requests, posting to channels or starting Sessions.',
};
function describeMode(mode) { $('mode-description').textContent = modeDescriptions[mode] ?? ''; }

// Theme is a per-browser preference shared with the sidebar switch; theme.js applies it.
const themeInputs = [...document.querySelectorAll('input[name="theme"]')];
let theme = 'system';
try { theme = JSON.parse(localStorage.getItem('flock-theme')) ?? 'system'; } catch {}
for (const input of themeInputs) {
  input.checked = input.value === theme;
  input.onchange = () => {
    if (!input.checked) return;
    try { localStorage.setItem('flock-theme', JSON.stringify(input.value)); } catch {}
    document.dispatchEvent(new CustomEvent('flock-theme', { detail: input.value }));
  };
}

async function load() {
  const response = await fetch('/api/settings');
  if (!response.ok) throw new Error();
  const {mode, rules, canManagePolicy, t3, model, t3Setup} = await response.json();
  $('model-section').hidden = !model;
  $('model').replaceChildren();
  renderModel(model);
  currentMode = mode;
  for (const input of modeInputs) {
    input.checked = input.value === mode;
    input.disabled = !canManagePolicy;
  }
  $('mode-options').classList.toggle('read-only', !canManagePolicy);
  describeMode(mode);
  $('rules').replaceChildren();
  if (!rules.length) row('rules', canManagePolicy
    ? emptyLabel('No rules yet', 'Choosing Always allow on a request adds one here. You can also run ', command('flock rules add'), '.')
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
    remove.onclick = () => void submit('rule', {kind, action, target, add: false}, [remove]);
    item.append(remove);
  }
  $('connections').replaceChildren();
  if (t3) {
    for (const environment of t3.environments) row('connections', emptyLabel(environment, 'Sessions in this environment can ask your Bots'));
    row('connections', emptyLabel('Coding agents', 'Codex and Claude Code on this machine reach Flock on port ', command(String(t3.port)), '.'));
  } else if (canManagePolicy && t3Setup) {
    row('connections', emptyLabel('Not connected', 'Connect a T3 environment so its Sessions can ask your Bots.'));
  } else if (canManagePolicy) {
    row('connections', emptyLabel('Not connected',
      'To let T3 Sessions ask your Bots, run ', command('flock t3 connect ENVIRONMENT MCP_URL'), ' on this machine.'));
  } else {
    row('connections', emptyLabel('No connection details available', 'The person who connected T3 and installation administrators can see its details.'));
  }
  $('rule-form').hidden = !canManagePolicy;
  renderT3Setup(t3Setup);
}

// One section shows at a time, chosen by the URL hash, like a settings window's sidebar.
const sectionNames = { general: 'General', permissions: 'Permissions', bots: 'Bots', connections: 'Connections' };
function showSection() {
  const section = Object.hasOwn(sectionNames, location.hash.slice(1)) ? location.hash.slice(1) : 'general';
  for (const panel of document.querySelectorAll('[data-panel]')) panel.hidden = panel.dataset.panel !== section;
  for (const link of document.querySelectorAll('[data-section]')) {
    if (link.dataset.section === section) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  }
  $('crumb').textContent = sectionNames[section];
  document.title = `${sectionNames[section]} · Flock settings`;
}
addEventListener('hashchange', showSection);
showSection();

for (const [action, label] of Object.entries(actionLabels)) {
  const option = document.createElement('option');
  option.value = action;
  option.textContent = label;
  $('rule-action').append(option);
}
$('rule-form').onsubmit = async (event) => {
  event.preventDefault();
  const form = event.target;
  const data = Object.fromEntries(new FormData(form));
  if (await submit('rule', { ...data, target: data.target.trim(), add: true }, [...form.elements])) form.reset();
};

function link(label, href) {
  const anchor = document.createElement('a');
  anchor.className = 'button';
  anchor.textContent = label;
  anchor.href = href;
  return anchor;
}
async function loadBots() {
  const response = await fetch('/api/bots');
  if (!response.ok) throw new Error();
  const bots = await response.json();
  $('bots').replaceChildren();
  if (!bots.length) row('bots', emptyLabel('No Bots yet', 'Create one from the + button in the conversation list.'));
  for (const bot of bots) {
    const item = row('bots', emptyLabel(bot.name, 'Name, look, role and recurring requests'));
    const actions = document.createElement('span');
    actions.className = 'row-actions';
    actions.append(link('Edit', `/?${new URLSearchParams({ bot: bot.id, settings: '1' })}`),
      link('Sharing', `/settings/${encodeURIComponent(bot.id)}`));
    item.append(actions);
  }
}
loadBots().catch(() => row('bots', emptyLabel('Could not load Bots', 'Reload the page to try again.')));

// ChatGPT sign-in runs on the machine running Flock; this page polls until it settles.
let modelPoll = 0;
function actionButton(item, label, onclick, primary = false) {
  const control = document.createElement('button');
  control.type = 'button';
  control.className = primary ? 'button primary' : 'button';
  control.textContent = label;
  control.onclick = onclick;
  item.append(control);
  return control;
}
async function signIn(reconsent, control) {
  control.disabled = true;
  try {
    const response = await fetch('/api/settings/model-login', {
      method: 'POST', headers: {'Content-Type': 'application/json', 'X-Flock-Action': '1'},
      body: JSON.stringify({ reconsent }),
    });
    if (!response.ok) throw new Error();
  } catch {
    control.disabled = false;
    $('status').textContent = 'Could not start ChatGPT sign-in.';
    return;
  }
  await load().catch(() => {});
}
function renderModel(model) {
  clearTimeout(modelPoll);
  if (!model) return;
  if (model.demo) {
    row('model', emptyLabel('Demo model', 'This Flock runs with ', command('--fixed'), ', so Bots use a local demo instead of ChatGPT.'));
    return;
  }
  if (model.signingIn) {
    row('model', emptyLabel('Waiting for ChatGPT…', 'Finish signing in in the browser window that opened on this machine. This page updates when you’re done.'));
    modelPoll = setTimeout(() => void load().catch(() => {}), 2000);
    return;
  }
  if (model.signInError) row('model', emptyLabel('Sign-in didn’t finish', model.signInError)).classList.add('settings-row-error');
  if (model.unavailable) {
    row('model', emptyLabel('ChatGPT', 'Flock couldn’t check the connection right now. Reload to try again.'));
  } else if (model.connected) {
    row('model', emptyLabel('ChatGPT', model.account ? `Connected as ${model.account}` : 'Connected'));
    if (!model.planUsage) {
      const item = row('model', emptyLabel('Plan usage not granted', 'Bots can’t use your ChatGPT plan until you allow it.'));
      if (model.canSignIn) actionButton(item, 'Allow plan usage', (event) => void signIn(true, event.currentTarget), true);
    }
  } else {
    const item = row('model', emptyLabel('ChatGPT not connected', 'Sign in so your Bots can use your ChatGPT plan.'));
    if (model.canSignIn) actionButton(item, 'Sign in with ChatGPT', (event) => void signIn(false, event.currentTarget), true);
  }
}

// Connecting T3 runs on the machine running Flock; the page shows T3's authorization link and polls.
let t3Poll = 0;
function renderT3Setup(setup) {
  clearTimeout(t3Poll);
  $('t3-form').hidden = !setup || setup.running;
  if (!setup) return;
  if (setup.running) {
    const item = row('connections', emptyLabel(setup.authorizationUrl ? 'Authorize Flock in T3' : 'Connecting to T3…',
      setup.authorizationUrl ? 'Approve read and operate access in T3. This page updates when you’re done.' : 'Starting T3 authorization.'));
    if (setup.authorizationUrl) {
      const open = link('Open T3', setup.authorizationUrl);
      open.classList.add('primary');
      open.target = '_blank';
      open.rel = 'noopener noreferrer';
      item.append(open);
    }
    t3Poll = setTimeout(() => void load().catch(() => {}), 2000);
  } else if (setup.error) {
    row('connections', emptyLabel('Connection didn’t finish', setup.error)).classList.add('settings-row-error');
  }
}
$('t3-form').onsubmit = async (event) => {
  event.preventDefault();
  const form = event.target;
  const data = Object.fromEntries(new FormData(form));
  for (const control of form.elements) control.disabled = true;
  try {
    const response = await fetch('/api/settings/t3-connect', {
      method: 'POST', headers: {'Content-Type': 'application/json', 'X-Flock-Action': '1'},
      body: JSON.stringify({ environmentId: data.environmentId.trim(), mcpUrl: data.mcpUrl.trim() }),
    });
    if (!response.ok) throw new Error();
    form.reset();
  } catch { $('status').textContent = 'Could not start the T3 connection. Check the environment ID and MCP URL.'; }
  for (const control of form.elements) control.disabled = false;
  await load().catch(() => {});
};

async function submit(path, data, controls) {
  for (const control of controls) control.disabled = true;
  let saved = false;
  try {
    const response = await fetch('/api/settings/' + path, {
      method: 'POST', headers: {'Content-Type': 'application/json', 'X-Flock-Action': '1'},
      body: JSON.stringify(data)
    });
    saved = response.ok;
  } catch {}
  for (const control of controls) control.disabled = false;
  try {
    await load();
    $('status').textContent = saved ? 'Saved.' : 'Could not save this change.';
  } catch {
    for (const input of modeInputs) input.checked = input.value === currentMode;
    describeMode(currentMode);
    $('status').textContent = saved ? 'Saved. Reload to see the current settings.' : 'Could not reload settings.';
  }
  return saved;
}

for (const input of modeInputs) {
  input.onchange = async () => {
    if (!input.checked) return;
    describeMode(input.value);
    await submit('mode', {mode: input.value}, modeInputs);
  };
}
load().catch(() => { $('status').textContent = 'Could not load Flock settings.'; });
