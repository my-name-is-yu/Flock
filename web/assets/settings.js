import { actionLabel } from '/action-labels.mjs';

const $ = (id) => document.getElementById(id);
const modeInputs = [...document.querySelectorAll('input[name="mode"]')];
let currentMode;
let canManagePolicy = false;
let savingPolicy = false;
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
  supervised: 'By default, Bots ask before actions with outside effects, such as opening pull requests, posting to channels or starting Sessions.',
};
function describeMode(mode) { $('mode-description').textContent = modeDescriptions[mode] ?? ''; }
function reflectMode(mode) {
  for (const input of modeInputs) input.checked = input.value === mode;
  describeMode(mode);
}
function updatePolicyControls() {
  const disabled = savingPolicy || !canManagePolicy;
  for (const control of document.querySelectorAll('#mode-options input, #rules button')) control.disabled = disabled;
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
  const response = await fetch('/api/settings');
  if (!response.ok) throw new Error();
  const data = await response.json();
  const {mode, rules, t3} = data;
  canManagePolicy = data.canManagePolicy === true;
  currentMode = mode;
  $('mode-options').classList.toggle('read-only', !canManagePolicy);
  reflectMode(mode);
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
    remove.onclick = () => void submit('rule', {kind, action, target});
    item.append(remove);
  }
  $('connections').replaceChildren();
  if (t3) {
    for (const environment of t3.environments) row('connections', emptyLabel(environment, 'Sessions in this environment can ask your Bots'));
    row('connections', emptyLabel('Coding agents', 'Flock is configured to use port ', command(String(t3.port)),
      ' for local Codex and Claude Code connections.'));
  } else if (canManagePolicy) {
    row('connections', emptyLabel('Not connected',
      'To let T3 Sessions ask your Bots, run ', command('flock t3 connect ENVIRONMENT MCP_URL'), ' on the machine running Flock.'));
  } else {
    row('connections', emptyLabel('No connection details available', 'The person who connected T3 and installation administrators can see its details.'));
  }
  $('policy').hidden = false;
  updatePolicyControls();
}

async function submit(path, data) {
  if (!canManagePolicy || savingPolicy) return;
  savingPolicy = true;
  updatePolicyControls();
  let saved = false;
  try {
    const response = await fetch('/api/settings/' + path, {
      method: 'POST', headers: {'Content-Type': 'application/json', 'X-Flock-Action': '1'},
      body: JSON.stringify(data)
    });
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
}

for (const input of modeInputs) {
  input.onchange = async () => {
    if (!input.checked || savingPolicy || !canManagePolicy) return;
    describeMode(input.value);
    await submit('mode', {mode: input.value});
  };
}
load().catch(() => { $('status').textContent = 'Could not load Flock settings.'; });
