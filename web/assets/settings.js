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
  const {mode, rules, canManagePolicy, t3} = await response.json();
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
  } else if (canManagePolicy) {
    row('connections', emptyLabel('Not connected',
      'To let T3 Sessions ask your Bots, run ', command('flock t3 connect ENVIRONMENT MCP_URL'), ' on this machine.'));
  } else {
    row('connections', emptyLabel('No connection details available', 'The person who connected T3 and installation administrators can see its details.'));
  }
  $('rule-form').hidden = !canManagePolicy;
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
