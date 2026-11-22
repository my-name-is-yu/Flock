const [, , botId, section] = location.pathname.split('/').map(decodeURIComponent);
const $ = (id) => document.getElementById(id);
$('back').href = '/?' + new URLSearchParams({bot: botId});
document.querySelector('h1').textContent = botId + ' access settings';
const operations = {
  service: ['read','use','manage'], repository: ['read','write','pr','review','issues'],
  secret: ['use'], session: ['read'], mcp_server: ['discover','invoke','read_resource'],
  environment: ['inspect','configure','launch'], shared_memory: ['read','write'],
  workspace: ['read','write','exec'], web: ['search','fetch'], bot_admin: ['create_bot','rename_bot']
};
const resource = $('resource').elements;
resource.resourceKind.replaceChildren(...Object.keys(operations).map(k => new Option(k)));
function selectOperations() {
  resource.operation.replaceChildren(...operations[resource.resourceKind.value].map(k => new Option(k)));
}
resource.resourceKind.onchange = selectOperations;
selectOperations();
async function submit(path, data) {
  try {
    const response = await fetch('/api/access/' + path, {
      method: 'POST', headers: {'Content-Type':'application/json','X-Flock-Action':'1'},
      body: JSON.stringify({botId, ...data})
    });
    if (response.status === 409 && (await response.json()).error === 'configure_web_identities_first') {
      $('status').textContent = 'Configure FLOCK_WEB_IDENTITIES_FILE and restart Flock before granting people access.';
      return;
    }
    if (!response.ok) throw Error();
    await load();
    $('status').textContent = 'Saved.';
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
  b.onclick = action;
  row.append(b);
}
async function load() {
  const response = await fetch('/api/access?' + new URLSearchParams({bot:botId}));
  if (!response.ok) throw Error();
  const data = await response.json();
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
    for (const r of entries.filter(x => x.status === 'pending')) {
      const item = row(list,r.resourceKind ? scope(r) : [r.operation,r.target,r.name].join(' / '));
      for (const approve of [true,false]) button(item,approve?'Approve':'Deny',() => submit(endpoint,{requestId:r.id,approve}));
    }
  }
  for (const r of data.grants) button(row('grants',scope(r)),'Revoke',() => submit('resource',{...r,action:'revoke'}));
  for (const p of data.people) button(row('people',p.person+' / '+p.role),'Revoke',() => submit('person',{...p,action:'revoke'}));
}
try { await load(); document.getElementById(section)?.scrollIntoView(); }
catch { $('status').textContent = 'Bot administrator access is required.'; }
