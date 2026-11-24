import { accentColor } from '/avatars.js';

const [, , botId] = location.pathname.split('/').map(decodeURIComponent);
const $ = (id) => document.getElementById(id);
$('back').href = '/?' + new URLSearchParams({bot: botId});
fetch('/api/bots').then(r => r.ok ? r.json() : []).then(bots => {
  const bot = bots.find(item => item.id === botId);
  $('back-label').textContent = bot?.name || botId;
  if (bot) document.documentElement.style.setProperty('--bot-accent', accentColor(bot));
}).catch(() => { $('back-label').textContent = botId; });
function post(url, data) {
  return fetch(url, {
    method: 'POST', headers: {'Content-Type':'application/json','X-Flock-Action':'1'},
    body: JSON.stringify(data)
  });
}
async function submit(path, data) {
  try {
    const response = await post('/api/bot-settings/' + path, {botId, ...data});
    if (!response.ok) throw Error();
    await load();
    $('status').textContent = 'Saved.';
    return true;
  } catch {
    $('status').textContent = 'Could not save this change.';
    return false;
  }
}
$('members').onsubmit = (event) => {
  event.preventDefault();
  void submit('members', {add:true, ...Object.fromEntries(new FormData(event.target))});
};
function row(id, text) {
  const item = document.createElement('p');
  item.textContent = text;
  $(id).append(item);
  return item;
}
function button(row, label, action) {
  const b = document.createElement('button');
  b.type = 'button';
  b.textContent = label;
  b.className = /^(Deny|Decline|Remove|Disconnect)$/.test(label) ? 'button danger' : 'button';
  b.onclick = action;
  row.append(b);
  return b;
}
async function load() {
  const response = await fetch('/api/bot-settings?' + new URLSearchParams({bot:botId}));
  if (!response.ok) throw Error();
  const data = await response.json();
  $('administration').hidden = false;
  for (const id of ['inputs','people']) $(id).replaceChildren();
  for (const request of data.secretInputs.filter(x => x.status === 'pending')) {
    const item = row('inputs',request.name);
    const input = document.createElement('input');
    input.setAttribute('aria-label',request.name);
    input.type = 'password';
    input.autocomplete = 'new-password';
    item.append(input);
    button(item,'Save secret',() => {
      const value = input.value;
      input.value = '';
      void submit('secret',{requestId:request.id,value});
    });
  }
  $('secrets').hidden = !$('inputs').childElementCount;
  $('sharing').hidden = false;
  $('members').hidden = !data.canManageMembers;
  for (const person of data.members) {
    const item = row('people',person);
    if (data.canManageMembers) button(item,'Remove',() => submit('members',{person,add:false}));
  }
  if (!data.canManage) {
    note('people', 'Sharing managed by the owner', 'The Bot’s owner decides who else can use it.');
  } else if (!data.members.length) {
    if (data.canManageMembers)
      note('people', 'No shared members', 'Add a person ID below to share this Bot.');
    else
      note('people', 'No shared members', 'Enable shared sign-in in ',
        settingsLink('access', 'Flock settings'), ', then add the person’s ID here.');
  }
}
function settingsLink(section, text) {
  const element = document.createElement('a');
  element.href = '/settings#' + section;
  element.textContent = text;
  return element;
}
function rowLabel(title, ...detail) {
  const label = document.createElement('span');
  label.className = 'row-label';
  const heading = document.createElement('strong');
  heading.textContent = title;
  const sub = document.createElement('small');
  sub.append(...detail);
  label.append(heading, sub);
  return label;
}
function note(id, title, ...detail) {
  const item = document.createElement('p');
  item.append(rowLabel(title, ...detail));
  $(id).append(item);
}
const channelForm = $('channel-sender-form');
const channelSelect = $('channel-bridge');
let channelBridges = [];
let linkSaving = false;

function bridgeName(bridge) {
  return (bridge.type === 'slack' ? 'Slack' : 'iMessage') + ' · ' + bridge.id;
}
function channelRow(title, ...detail) {
  const item = document.createElement('div');
  item.className = 'settings-row';
  item.append(rowLabel(title, ...detail));
  $('channel-links').append(item);
  return item;
}
function renderChannelLinks(bridges) {
  const selected = channelBridges[channelSelect.value];
  channelBridges = bridges;
  $('channel-links').replaceChildren();
  channelSelect.replaceChildren();
  channelForm.hidden = !bridges.some(bridge => !bridge.hostManaged);
  if (!bridges.length) {
    channelRow('No channels connected', 'Set up Slack or iMessage in ',
      settingsLink('connections', 'Flock settings'), ' to link senders to this Bot.');
  } else if (channelForm.hidden) {
    channelRow('Managed on the Flock host', 'Sender links for these channels come from the host configuration.');
  } else if (!bridges.some(bridge => bridge.senders.length)) {
    channelRow('No senders linked', 'Link a sender to let them message this Bot from Slack or iMessage.');
  }
  for (const [index, bridge] of bridges.entries()) {
    if (!bridge.hostManaged) {
      const option = new Option(bridgeName(bridge), String(index));
      if (bridge.type === selected?.type && bridge.id === selected.id) option.selected = true;
      channelSelect.add(option);
    }
    for (const sender of bridge.senders) {
      const item = channelRow(sender.id, bridgeName(bridge), ' · ', sender.personId);
      if (!bridge.hostManaged) {
        const remove = button(item,'Remove',() => void submitChannelLink('removeSender',bridge,{id:sender.id}));
        remove.setAttribute('aria-label', 'Remove ' + sender.id + ' from ' + bridgeName(bridge));
      }
    }
  }
  if (bridges.some(bridge => bridge.pending)) {
    channelRow('Restart required', 'Flock uses the saved channel links after it restarts.');
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
  if (response.ok) renderChannelLinks((await response.json()).bridges);
}
async function loadMachines() {
  const response = await fetch('/api/channels/local-connections');
  if (!response.ok) throw Error();
  const connections = (await response.json()).filter(x =>
    x.botId === botId && ['pending','approved'].includes(x.status));
  $('local-connections').hidden = connections.length === 0;
  $('machines').replaceChildren();
  for (const connection of connections) {
    const item = document.createElement('div');
    $('machines').append(item);
    const title = document.createElement('p');
    title.textContent = connection.machineId + ' · ' + connection.threadId;
    item.append(title);
    const rules = document.createElement('ul');
    for (const rule of connection.rules) {
      const entry = document.createElement('li');
      entry.textContent = (rule.operation === 'read_file' ? 'Read ' : 'List ') + rule.path;
      rules.append(entry);
    }
    item.append(rules);
    const change = async (action, approve) => {
      try {
        const result = await post('/api/channels/local-connections/' + action, {id:connection.id, approve});
        if (!result.ok) throw Error();
        await loadMachines();
        $('status').textContent = 'Local connection updated.';
      } catch { $('status').textContent = 'Could not update this local connection.'; }
    };
    if (connection.status === 'pending') {
      button(item,'Approve',() => change('decide',true));
      button(item,'Decline',() => change('decide',false));
    } else button(item,'Disconnect',() => change('disconnect'));
  }
  return connections.length;
}
const [machines, admin] = await Promise.allSettled([loadMachines(), load(), loadChannelLinks()]);
if (machines.status === 'rejected') $('status').textContent = 'Could not load local machine connections.';
else if (admin.status === 'rejected' && !machines.value) $('status').textContent = 'Could not load Bot settings.';
document.getElementById(location.hash.slice(1))?.scrollIntoView();
