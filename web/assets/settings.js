const $ = (id) => document.getElementById(id);
const modeInputs = [...document.querySelectorAll('input[name="mode"]')];
const ruleLabels = {never: 'Never', ask: 'Ask first', allow: 'Allow'};
const actionLabels = {
  'bot.view': 'View Bot conversations',
  'bot.talk': 'Talk to Bots',
  'bot.decide': 'Respond to Bot requests',
  'bot.settings': 'Change Bot settings',
  'bot.create': 'Create Bots',
  'bot.rename': 'Rename Bots',
  'rules.manage': 'Manage permission rules',
  'repository.read': 'Read repositories',
  'repository.write': 'Write to repositories',
  'repository.pr': 'Open or update pull requests',
  'repository.review': 'Review pull requests',
  'repository.issues': 'Manage issues',
  'secret.use': 'Use secrets',
  'session.read': 'Read T3 Sessions',
  'mcp.discover': 'List MCP tools',
  'mcp.invoke': 'Call MCP tools',
  'mcp.read_resource': 'Read MCP resources',
  'environment.inspect': 'View T3 environments',
  'environment.configure': 'Configure T3 environments',
  'environment.launch': 'Start T3 Sessions',
  'shared_memory.read': 'Read shared memory',
  'shared_memory.write': 'Write to shared memory',
  'workspace.read': 'Read workspace files',
  'workspace.write': 'Write workspace files',
  'workspace.exec': 'Run workspace commands',
  'web.search': 'Search the web',
  'web.fetch': 'Open web pages',
  'skill.list': 'List skills',
  'skill.read': 'Read skills',
  'attachment.inspect': 'View attachments',
  'channel.use': 'Use channels',
  'channel.read': 'Read channel messages',
  'channel.post': 'Post channel messages',
  't3.coordinate': 'Coordinate T3 work',
  't3.send': 'Send messages to T3',
  'automation.change': 'Change automations',
  'email.send': 'Send email',
  'data.destructive_write': 'Make destructive changes',
};

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
  // Everyone sees both modes explained; only installation roots can switch.
  for (const input of modeInputs) {
    input.checked = input.value === mode;
    input.disabled = !canManagePolicy;
  }
  $('mode-options').classList.toggle('read-only', !canManagePolicy);
  $('rules').replaceChildren();
  if (!rules.length) row('rules', emptyLabel('No rules yet', 'Choosing Always allow on a request adds a rule here. You can also run flock rules add.'));
  for (const {kind, action, target} of rules) {
    const label = document.createElement('span');
    label.className = 'row-label';
    const title = document.createElement('strong');
    title.textContent = actionLabels[action] ?? action;
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
  } else {
    row('connections', emptyLabel('Not connected', 'Run flock t3 connect ENVIRONMENT MCP_URL on the machine running Flock.'));
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

for (const input of modeInputs) {
  input.onchange = async () => {
    if (!input.checked) return;
    const nextMode = input.value;
    if (!await submit('mode', {mode: nextMode}, modeInputs)) {
      for (const other of modeInputs) other.checked = other.value !== nextMode;
    }
  };
}
load().catch(() => { $('status').textContent = 'Could not load Flock settings.'; });
