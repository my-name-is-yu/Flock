import { accentColor } from '/avatars.js';
import { listenForBrowserNotifications } from '/browser-notifications.mjs';
import { actionButton, element, row, rowLabel, settingsNavigation } from '/settings-ui.js';

const botId = decodeURIComponent(location.pathname.split('/')[2]);
listenForBrowserNotifications(() => botId);
const botQuery = new URLSearchParams({bot: botId});
const $ = (id) => document.getElementById(id);
const status = $('status');
const navigation = settingsNavigation('/?' + botQuery);
navigation.get('bots').setAttribute('aria-current', 'page');

function showName(name) {
  $('crumb-bot').textContent = name;
  $('sharing-heading').textContent = `Who can use ${name}`;
  document.title = `${name} access · Flock settings`;
}
showName(botId);

async function loadBotName() {
  const response = await fetch('/api/bots');
  if (!response.ok) return;
  const bot = (await response.json()).find(item => item.id === botId);
  if (!bot) return;
  showName(bot.name);
  document.documentElement.style.setProperty('--bot-accent', accentColor(bot));
}
void loadBotName().catch(() => {});

function post(url, data) {
  return fetch(url, {
    method: 'POST', headers: {'Content-Type': 'application/json', 'X-Flock-Action': '1'},
    body: JSON.stringify(data),
  });
}

async function save(section, url, data, refresh, success) {
  const controls = $(section).querySelectorAll('button, input');
  for (const control of controls) control.disabled = true;
  try {
    if (!(await post(url, data)).ok) throw new Error();
    try {
      await refresh();
      status.textContent = success;
    } catch {
      status.textContent = 'Saved. Reload this page to see the change.';
    }
    return true;
  } catch {
    status.textContent = 'Couldn’t save this change.';
    return false;
  } finally {
    for (const control of controls) control.disabled = false;
  }
}

function settingsLink(section, text) {
  const link = element('a', text);
  link.href = navigation.get(section).href;
  return link;
}

async function readAccess() {
  const response = await fetch('/api/bot-settings?' + botQuery);
  if (!response.ok) throw new Error('', {cause: response.status});
  return response.json();
}

function saveMember(person, add) {
  return save('sharing', '/api/bot-settings/members', {botId, person, add},
    async () => renderPeople(await readAccess()), add ? 'Person added.' : 'Person removed.');
}

const memberForm = $('members');
memberForm.onsubmit = async (event) => {
  event.preventDefault();
  const person = memberForm.elements.person.value.trim();
  if (!person) {
    status.textContent = 'Enter a person’s ID.';
    return;
  }
  if (await saveMember(person, true)) memberForm.reset();
};

function renderPeople({canManage, canManageMembers, members}) {
  const people = $('people');
  people.replaceChildren();
  memberForm.hidden = !canManageMembers;
  if (!canManage) {
    row(people, rowLabel('Managed by the owner', 'The Bot’s owner decides who else can use it.'));
  } else if (!members.length) {
    const detail = canManageMembers ? ['Add a person’s ID below to share this Bot with them.']
      : ['Enable shared sign-in in ', settingsLink('access', 'Flock settings'), ' to share this Bot.'];
    row(people, rowLabel('No one added', ...detail));
  }
  for (const person of members) {
    const item = row(people, rowLabel(person));
    if (canManageMembers) {
      const remove = actionButton(item, 'Remove', () => void saveMember(person, false));
      remove.classList.add('quiet-danger');
      remove.setAttribute('aria-label', `Remove ${person}`);
    }
  }
  $('sharing').hidden = false;
}

function renderSecrets({secretInputs}) {
  const inputs = $('inputs');
  inputs.replaceChildren();
  for (const {id, name, status} of secretInputs) {
    if (status !== 'pending') continue;
    const item = row(inputs, rowLabel(name));
    const actions = element('form', '', 'row-actions secret-form');
    const input = element('input');
    input.setAttribute('aria-label', name);
    input.type = 'password';
    input.autocomplete = 'new-password';
    input.required = true;
    const button = element('button', 'Save', 'button primary');
    actions.append(input, button);
    actions.onsubmit = async (event) => {
      event.preventDefault();
      if (await save('secrets', '/api/bot-settings/secret', {botId, requestId: id, value: input.value},
        async () => renderSecrets(await readAccess()), 'Secret saved.')) input.value = '';
    };
    item.append(actions);
  }
  $('secrets').hidden = !inputs.childElementCount;
}

async function loadAccess() {
  const access = await readAccess();
  renderSecrets(access);
  renderPeople(access);
}

const channelForm = $('channel-sender-form');
const channelSelect = $('channel-bridge');
let channelBridges = [];
let linkSaving = false;

function bridgeName(bridge) {
  return (bridge.type === 'slack' ? 'Slack' : 'iMessage') + ' · ' + bridge.id;
}
function renderChannelLinks(bridges) {
  const selected = channelBridges[channelSelect.value];
  channelBridges = bridges;
  $('channel-links').replaceChildren();
  channelSelect.replaceChildren();
  channelForm.hidden = !bridges.some(bridge => !bridge.hostManaged);
  if (!bridges.length) {
    row('channel-links', rowLabel('No channels connected', 'Set up Slack or iMessage in ',
      settingsLink('connections', 'Flock settings'), ' to link senders to this Bot.'));
  } else if (channelForm.hidden) {
    row('channel-links', rowLabel('Managed by the server', 'Sender links for these channels are set on the computer running Flock.'));
  } else if (!bridges.some(bridge => bridge.senders.length)) {
    row('channel-links', rowLabel('No senders linked', 'Link a sender to let them message this Bot from Slack or iMessage.'));
  }
  for (const [index, bridge] of bridges.entries()) {
    if (!bridge.hostManaged) {
      const option = new Option(bridgeName(bridge), String(index));
      if (bridge.type === selected?.type && bridge.id === selected.id) option.selected = true;
      channelSelect.add(option);
    }
    for (const sender of bridge.senders) {
      const item = row('channel-links', rowLabel(sender.id, bridgeName(bridge), ' · ', sender.personId));
      if (!bridge.hostManaged) {
        const remove = actionButton(item, 'Remove', () => void submitChannelLink('removeSender', bridge, {id: sender.id}));
        remove.classList.add('quiet-danger');
        remove.setAttribute('aria-label', 'Remove ' + sender.id + ' from ' + bridgeName(bridge));
      }
    }
  }
  if (bridges.some(bridge => bridge.pending)) {
    row('channel-links', rowLabel('Restart required', 'Flock uses the saved channel links after it restarts.'));
  }
  $('channel-senders').hidden = false;
}
function setLinkSaving(saving) {
  linkSaving = saving;
  for (const control of $('channel-senders').querySelectorAll('button, select, input')) {
    control.disabled = saving;
  }
}
async function submitChannelLink(action, bridge, sender) {
  if (linkSaving) return;
  setLinkSaving(true);
  try {
    const response = await post('/api/bot-settings/channels', {botId,action,type:bridge.type,id:bridge.id,sender});
    const data = await response.json();
    if (!response.ok) {
      $('status').textContent = data.message || 'Could not update the channel link.';
      return;
    }
    renderChannelLinks(data.bridges);
    if (action === 'setSender') {
      channelForm.elements.sender.value = '';
      channelForm.elements.person.value = '';
    }
    $('status').textContent = 'Channel links saved.';
  } catch {
    $('status').textContent = 'Could not update the channel link.';
    return;
  } finally {
    setLinkSaving(false);
  }
  if (action === 'setSender') channelForm.elements.sender.focus();
}
channelForm.onsubmit = (event) => {
  event.preventDefault();
  const bridge = channelBridges[channelSelect.value];
  const id = channelForm.elements.sender.value.trim();
  const personId = channelForm.elements.person.value.trim();
  if (!bridge || !id || !personId) {
    $('status').textContent = 'Choose a channel and enter both sender and person IDs.';
    return;
  }
  void submitChannelLink('setSender',bridge,{id,personId});
};
async function loadChannelLinks() {
  const response = await fetch('/api/bot-settings/channels?' + new URLSearchParams({bot:botId}));
  if (response.status === 403 || response.status === 404) return;
  if (!response.ok) throw new Error();
  renderChannelLinks((await response.json()).bridges);
}

function changeMachine(action, data, success) {
  return save('local-connections', '/api/channels/local-connections/' + action, data, loadMachines, success);
}

const operationLabels = new Map([['read_file', 'Read'], ['list_directory', 'List']]);
async function loadMachines() {
  const response = await fetch('/api/channels/local-connections');
  if (!response.ok) throw new Error();
  const connections = (await response.json()).filter(connection => connection.botId === botId &&
    (connection.status === 'pending' || connection.status === 'approved'));
  const rows = document.createDocumentFragment();
  for (const {id, machineId, threadId, rules, status} of connections) {
    const paths = element('ul', '', 'machine-paths');
    for (const {operation, path} of rules) {
      const label = operationLabels.get(operation);
      if (!label) throw new Error();
      const entry = element('li', label + ' ');
      entry.append(element('code', path));
      paths.append(entry);
    }
    const session = `Session ${threadId}`;
    const label = rowLabel(machineId, session);
    label.append(paths);
    const item = row(rows, label);
    const actions = element('span', '', 'row-actions');
    actions.setAttribute('role', 'group');
    actions.setAttribute('aria-label', `${machineId} · ${session}`);
    if (status === 'pending') {
      const decline = actionButton(actions, 'Decline', () => void changeMachine('decide', {id, approve: false}, 'Connection declined.'));
      decline.classList.add('quiet-danger');
      actionButton(actions, 'Approve', () => void changeMachine('decide', {id, approve: true}, 'Connection approved.'), true);
    } else {
      const disconnect = actionButton(actions, 'Disconnect', () => void changeMachine('disconnect', {id}, 'Computer disconnected.'));
      disconnect.classList.add('quiet-danger');
    }
    item.append(actions);
  }
  $('machines').replaceChildren(rows);
  $('local-connections').hidden = !connections.length;
  return connections.length;
}

const [machines, access, channels] = await Promise.allSettled([loadMachines(), loadAccess(), loadChannelLinks()]);
if (machines.status === 'rejected') status.textContent = 'Couldn’t load connected computers.';
else if (access.status === 'rejected' && (access.reason.cause !== 403 || !machines.value)) {
  status.textContent = 'Couldn’t load this Bot’s access.';
} else if (channels.status === 'rejected') status.textContent = 'Couldn’t load channel senders.';
const target = $(location.hash.slice(1));
if (target && !target.hidden) target.scrollIntoView({block: 'nearest'});
