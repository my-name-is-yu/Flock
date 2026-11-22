const [, , botId, section] = location.pathname.split('/').map(decodeURIComponent);
const $ = (id) => document.getElementById(id);
$('back').href = '/?' + new URLSearchParams({bot: botId});
const resource = $('resource').elements;
function configureResourceForm(operations) {
  const kinds = Object.keys(operations);
  resource.resourceKind.replaceChildren(...kinds.map(kind => new Option(kind)));
  const selectOperations = () => {
    $('resource-hint').textContent = resource.resourceKind.value==='session'
      ? 'Use the environment and thread IDs, for example ["local","thread-id"].' : '';
    resource.operation.replaceChildren(...operations[resource.resourceKind.value].map(op => new Option(op)));
  };
  resource.resourceKind.onchange = selectOperations;
  selectOperations();
}
function post(url, data) {
  return fetch(url, {
    method: 'POST', headers: {'Content-Type':'application/json','X-Flock-Action':'1'},
    body: JSON.stringify(data)
  });
}
async function submit(path, data) {
  try {
    const response = await post('/api/access/' + path, {botId, ...data});
    if (response.status === 409 && (await response.json()).error === 'configure_web_identities_first') {
      $('status').textContent = 'Configure FLOCK_WEB_IDENTITIES_FILE and restart Flock before granting people access.';
      return;
    }
    if (!response.ok) throw Error();
    await load();
    $('status').textContent = response.status === 202 ? 'Approved. Retry applying this Bot change.' : 'Saved.';
  } catch { $('status').textContent = 'Could not save this change. Check your access and refresh.'; }
}
for (const path of ['person','resource']) {
  $(path).onsubmit = (event) => {
    event.preventDefault();
    void submit(path, {action:'grant', ...Object.fromEntries(new FormData(event.target))});
  };
}
function row(id, text) {
  const item = document.createElement('p');
  item.textContent = text;
  $(id).append(item);
  return item;
}
function button(row, label, action) {
  const b = document.createElement('button');
  b.textContent = label;
  b.className = /^(Deny|Decline|Revoke|Disconnect)$/.test(label) ? 'button danger' : 'button';
  b.onclick = action;
  row.append(b);
}
async function load() {
  const response = await fetch('/api/access?' + new URLSearchParams({bot:botId}));
  if (!response.ok) throw Error();
  const data = await response.json();
  $('administration').hidden = false;
  configureResourceForm(data.operations);
  for (const id of ['inputs','requests','changes','grants','people']) $(id).replaceChildren();
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
  const scope = r => [r.resourceKind,r.resourceId,r.operation].join(' / ');
  for (const [list, endpoint, entries] of [['requests','decision',data.requests],['changes','bot-change',data.botChanges]]) {
    for (const r of entries.filter(x => x.status === 'pending' || (endpoint === 'bot-change' && x.status === 'approved'))) {
      const item = row(list,r.resourceKind ? scope(r) : [r.operation,r.target,r.name].join(' / '));
      if (r.status === 'approved') {
        button(item,'Apply approved change',() => submit(endpoint,{requestId:r.id,approve:true}));
        continue;
      }
      for (const approve of [true,false]) button(item,approve?'Approve':'Deny',() => submit(endpoint,{requestId:r.id,approve}));
    }
  }
  for (const [section, list] of [['secrets','inputs'],['access-requests','requests'],['bot-changes','changes']]) $(section).hidden = !$(list).childElementCount;
  for (const r of data.grants) button(row('grants',scope(r)),'Revoke',() => submit('resource',{resourceKind:r.resourceKind,resourceId:r.resourceId,operation:r.operation,action:'revoke'}));
  for (const p of data.people) button(row('people',p.person+' / '+p.role),'Revoke',() => submit('person',{...p,action:'revoke'}));
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
else if (admin.status === 'rejected' && !machines.value) $('status').textContent = 'Bot administrator access is required.';
document.getElementById(location.hash.slice(1) || section)?.scrollIntoView();
