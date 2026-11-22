const $ = (id) => document.getElementById(id);
const status = $('status');
const savedChannel = new URLSearchParams(location.search).get('channel') ?? '';
let channel = /^[1-9][0-9]*$/.test(savedChannel) ? savedChannel : /^#channel=([1-9][0-9]*)$/.exec(location.hash)?.[1];
let busy = false;
let question;
const labels = {
  prepared: 'Prepared', accepted: 'Accepted', providerPending: 'Provider pending', applied: 'Applied',
  failed: 'Failed', rejected: 'Rejected', unknown: 'Unknown',
};

async function request(path, data) {
  const options = data === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Flock-Action': '1' },
    body: JSON.stringify(data),
  };
  const response = await fetch('/api/session-handoffs' + path, options);
  if (!response.ok) throw new Error('Handoff unavailable');
  return response.json();
}

function fields(id, entries) {
  $(id).replaceChildren(...entries.flatMap(([label, value]) => {
    const term = document.createElement('dt');
    const detail = document.createElement('dd');
    term.textContent = label;
    detail.textContent = value ?? '—';
    return [term, detail];
  }));
}

function list(id, items) {
  $(id).replaceChildren(...items.map((text) => {
    const item = document.createElement('li');
    item.textContent = text;
    return item;
  }));
}

function workState(value) {
  return String(value ?? '').replaceAll('_', ' ');
}

function workAnswer(view, work, stage, prompt) {
  const form = document.createElement('form');
  const label = document.createElement('label');
  label.textContent = prompt.text;
  const input = document.createElement('textarea');
  input.rows = 2;
  input.required = true;
  input.maxLength = 8000;
  input.autocomplete = 'off';
  label.append(input);
  const button = document.createElement('button');
  button.textContent = 'Send answer';
  form.append(label, button);
  form.onsubmit = (event) => {
    event.preventDefault();
    void run(async () => {
      render(await request(`/${channel}/work-answer`, {
        generation: view.generation, delegationId: work.delegationId, stageId: stage,
        waitId: prompt.waitId, text: input.value,
      }));
      status.textContent = 'Answer recorded.';
    }, 'Could not record this work answer. Refresh the handoff and try again.');
  };
  return form;
}

function render(view) {
  const coordination = view.mode === 'coordinate';
  const takingBack = view.sealed || view.frozen;
  $('handoff').hidden = false;
  $('handoff-heading').textContent = `${coordination ? 'Coordination' : 'Consultation'} with ${view.botId}`;
  $('state').textContent = view.released ? 'Released' : takingBack ? 'Take-back in progress' :
    view.opening ? 'Opening · awaiting T3 activation' : !coordination && view.reply?.state === 'applied' ? 'Replied' : 'Active';
  fields('origin', [
    ['Invited by', view.person], ['Environment', view.target.environmentId], ['Project', view.target.projectId],
    ['Session', view.target.threadId], ['Original Run', view.target.runId], ['Source message', view.sourceMessageId],
    ['Grant epoch', view.grantEpoch], ['Local generation', view.generation],
    ...(coordination ? [['Goal', view.goal], ['Scope', view.allowDelegation ? 'Guide and delegate' : 'Guide']] : []),
  ]);
  fields('barriers', [
    ['T3 root admission', view.sealed ? 'Sealed' : view.opening ? 'Awaiting activation' : 'Seal unconfirmed'],
    ['Flock work admission', view.frozen ? 'Frozen' : 'Freeze unconfirmed'],
    ...(coordination ? [['T3 knows of freeze', view.releaseAcknowledged ? 'Confirmed' : 'Unconfirmed']] : []),
  ]);
  const commandCount = view.residualCommandIds.length;
  $('residual').hidden = commandCount === 0;
  $('residual-count').textContent = `${commandCount} root ${commandCount === 1 ? 'command' : 'commands'} admitted before the seal`;
  list('residual-commands', view.residualCommandIds);
  const childCount = view.admittedChildIds.length;
  $('residual-work').hidden = !view.frozen || childCount === 0;
  $('residual-work-count').textContent = `${childCount} child ${childCount === 1 ? 'launch' : 'launches'} admitted before the freeze`;
  list('residual-children', view.admittedChildIds);
  $('delegated').hidden = view.work.length === 0;
  const expanded = new Set(Array.from($('work-list').querySelectorAll('details[open]'), (item) => item.dataset.delegation));
  const workers = new Map(view.work.map((work) => [work.delegationId, work.executor]));
  $('work-list').replaceChildren(...view.work.map((work) => {
    const item = document.createElement('li');
    const summary = document.createElement('details');
    summary.dataset.delegation = work.delegationId;
    summary.open = expanded.has(work.delegationId) || !!work.question || work.stages.some((stage) => stage.question);
    const title = document.createElement('summary');
    title.textContent = `${work.executor} · ${workState(work.result?.status ?? work.state)}`;
    const content = document.createElement('ul');
    const notes = [work.purpose, `Requested by: ${workers.get(work.parentDelegationId) ?? view.botId}`,
      `Required evidence: ${work.doneWhen}`, `Input commit: ${work.commit}`,
      ...(work.result ? [work.result.report, ...work.result.evidence.map((source) =>
        [source.stageId, source.verdict, source.findings, source.threadId, source.runId, source.itemId]
          .filter(Boolean).join(' · '))] : []),
    ];
    for (const note of notes) {
      const row = document.createElement('li');
      row.textContent = note;
      content.append(row);
    }
    for (const stage of work.stages) {
      const row = document.createElement('li');
      row.textContent = `${stage.name} · ${workState(stage.status)}`;
      if (!takingBack && stage.question) row.append(workAnswer(view,work,stage.stageId,stage.question));
      content.append(row);
    }
    summary.append(title, content);
    if (!takingBack && work.question) summary.append(workAnswer(view,work,null,work.question));
    item.append(summary);
    return item;
  }));
  fields('reply', [
    ['Delivery', view.reply ? labels[view.reply.state] ?? view.reply.state : 'No reply yet'],
    ...(view.reply?.provider?.nativeItemId ? [['Provider turn', view.reply.provider.nativeTurnId],
      ['Provider input', view.reply.provider.nativeItemId]] : []),
  ]);
  $('decision').hidden = !view.latestDecision;
  $('decision-text').textContent = view.latestDecision?.text ?? '';
  $('consult').hidden = coordination || takingBack || view.opening || ['applied','failed','rejected'].includes(view.reply?.state);
  $('freeze').hidden = !coordination || view.frozen || view.opening;
  question = view.pendingQuestion ? { generation: view.generation, eventId: view.pendingQuestion.eventId } : undefined;
  $('answer').hidden = !question;
  $('question').textContent = view.pendingQuestion?.text ?? '';
}

async function run(work, message) {
  if (busy) return;
  busy = true;
  for (const button of document.querySelectorAll('button')) button.disabled = true;
  status.textContent = '';
  try { await work(); }
  catch { status.textContent = message; }
  finally {
    busy = false;
    for (const button of document.querySelectorAll('button')) button.disabled = false;
  }
}

async function loadRecent() {
  const recent = await request('');
  $('recent').hidden = recent.length === 0;
  $('recent-links').replaceChildren(...recent.map((item) => {
    const row = document.createElement('li');
    const link = document.createElement('a');
    link.href = '/session-handoff.html?channel=' + item.channelId;
    link.textContent = `${item.mode === 'coordinate' ? 'Coordination' : 'Consultation'} with ${item.botId}`;
    link.title = item.runId;
    row.append(link);
    return row;
  }));
}

$('accept').addEventListener('submit', (event) => {
  event.preventDefault();
  const field = event.currentTarget.elements.invitation;
  const text = field.value;
  field.value = '';
  void run(async () => {
    const invited = await request('', JSON.parse(text));
    channel = String(invited.channelId);
    history.replaceState(null, '', '#channel=' + channel);
    render(invited);
    await loadRecent();
  }, 'Could not accept the invitation. Copy it from T3 and check your Flock access.');
});

for (const [id, action] of [['reconcile', 'resume'], ['consult', 'consult'], ['freeze', 'freeze']]) {
  $(id).addEventListener('click', () => void run(async () => {
    const response = await request(`/${channel}/${action}`, {});
    render(action === 'consult' ? await request(`/${channel}`) : response);
  }, 'Could not update the handoff. Check the connection and try again.'));
}

$('answer').addEventListener('submit', (event) => {
  event.preventDefault();
  if (!question) return;
  const input = event.currentTarget.elements.text;
  const answer = { ...question, text: input.value };
  void run(async () => {
    const view = await request(`/${channel}/answer`, answer);
    input.value = '';
    render(view);
    status.textContent = 'Answer recorded.';
  }, 'Could not record the answer. Refresh the handoff and try again.');
});

void loadRecent().catch(() => {});
if (channel) void run(async () => render(await request(`/${channel}`)), 'This handoff is unavailable.');
