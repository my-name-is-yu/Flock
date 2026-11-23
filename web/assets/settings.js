const $ = (id) => document.getElementById(id);
const supervised = $('supervised');
const ruleLabels = {never: 'Never', ask: 'Ask first', allow: 'Allow'};

function row(list, text) {
  const item = document.createElement('p');
  item.textContent = text;
  $(list).append(item);
  return item;
}

async function load() {
  const response = await fetch('/api/settings');
  if (!response.ok) throw new Error();
  const {mode, rules, canManagePolicy, t3} = await response.json();
  $('mode').textContent = mode === 'supervised' ? 'Supervised' : 'Full';
  $('mode').hidden = canManagePolicy;
  $('mode-control').hidden = !canManagePolicy;
  supervised.checked = mode === 'supervised';
  $('rules').replaceChildren();
  if (!rules.length) row('rules', 'None');
  for (const {kind, action, target} of rules) {
    const item = row('rules', [ruleLabels[kind], action, target].join(' · '));
    if (!canManagePolicy) continue;
    const remove = document.createElement('button');
    remove.textContent = 'Remove';
    remove.className = 'button danger';
    remove.onclick = () => void submit('rule', {kind, action, target}, [remove]);
    item.append(remove);
  }
  $('connections').replaceChildren();
  if (t3) {
    row('connections', `Port ${t3.port}`);
    for (const environment of t3.environments) row('connections', environment);
  } else {
    row('connections', 'No connected environments');
  }
  $('policy').hidden = false;
}

async function submit(path, data, controls) {
  for (const control of controls) control.disabled = true;
  try {
    const response = await fetch('/api/settings/' + path, {
      method: 'POST', headers: {'Content-Type': 'application/json', 'X-Flock-Action': '1'},
      body: JSON.stringify(data)
    });
    if (!response.ok) throw new Error();
    await load();
    $('status').textContent = 'Saved.';
    return true;
  } catch {
    $('status').textContent = 'Could not save this change.';
    return false;
  } finally {
    for (const control of controls) control.disabled = false;
  }
}

supervised.onchange = async () => {
  const nextMode = supervised.checked ? 'supervised' : 'full';
  if (!await submit('mode', {mode: nextMode}, [supervised])) supervised.checked = nextMode === 'full';
};
load().catch(() => { $('status').textContent = 'Could not load Flock settings.'; });
