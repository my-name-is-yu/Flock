import { actionLabel } from '/action-labels.mjs';
const $ = (id) => document.getElementById(id);
const modeInputs = [...document.querySelectorAll('input[name="mode"]')];
let currentMode;
const ruleLabels = {never: 'Never', ask: 'Ask first', allow: 'Allow'};


function emptyLabel(title, detail) {
  const label = document.createElement('span');
  label.className = 'row-label';
  const heading = document.createElement('strong');
  heading.textContent = title;
  const sub = document.createElement('small');
  sub.textContent = detail;
  label.append(heading, sub);
  return label;
}

function row(list, content) {
  const item = document.createElement('p');
  item.append(content);
  $(list).append(item);
  return item;
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
  $('rules').replaceChildren();
  if (!rules.length) row('rules', emptyLabel('No rules yet', canManagePolicy
    ? 'Choose Always allow on a request or run flock rules add.'
    : 'An installation administrator can add rules.'));
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
    remove.className = 'button danger';
    remove.onclick = () => void submit('rule', {kind, action, target}, [remove]);
    item.append(remove);
  }
  $('connections').replaceChildren();
  if (t3) {
    row('connections', emptyLabel('Flock MCP endpoint', `Port ${t3.port}`));
    for (const environment of t3.environments) row('connections', emptyLabel(environment, 'Connected environment'));
  } else if (canManagePolicy) {
    row('connections', emptyLabel('Not connected', 'Run flock t3 connect ENVIRONMENT MCP_URL on the machine running Flock.'));
  } else {
    row('connections', emptyLabel('No connection details available', 'The person who connected T3 and installation administrators can see its details.'));
  }
  $('policy').hidden = false;
}

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
  try {
    await load();
    $('status').textContent = saved ? 'Saved.' : 'Could not save this change.';
  } catch {
    for (const input of modeInputs) input.checked = input.value === currentMode;
    $('status').textContent = saved ? 'Saved. Reload to see the current settings.' : 'Could not reload settings.';
  }
}

for (const input of modeInputs) {
  input.onchange = async () => {
    if (input.checked) await submit('mode', {mode: input.value}, modeInputs);
  };
}
load().catch(() => { $('status').textContent = 'Could not load Flock settings.'; });
