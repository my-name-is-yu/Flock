const [, , botId] = location.pathname.split('/').map(decodeURIComponent);
const $ = (id) => document.getElementById(id);
$('back').href = '/?' + new URLSearchParams({bot: botId});
fetch('/api/bots').then(r => r.ok ? r.json() : []).then(bots => {
  $('back-label').textContent = bots.find(bot => bot.id === botId)?.name || botId;
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
  b.textContent = label;
  b.className = /^(Deny|Decline|Remove|Disconnect)$/.test(label) ? 'button danger' : 'button';
  b.onclick = action;
  row.append(b);
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
  $('sharing').hidden = !data.canManageMembers;
  for (const person of data.members) {
    button(row('people',person),'Remove',() => submit('members',{person,add:false}));
  }
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
const [machines, admin] = await Promise.allSettled([loadMachines(), load()]);
if (machines.status === 'rejected') $('status').textContent = 'Could not load local machine connections.';
else if (admin.status === 'rejected' && !machines.value) $('status').textContent = 'Could not load Bot settings.';
document.getElementById(location.hash.slice(1))?.scrollIntoView();
