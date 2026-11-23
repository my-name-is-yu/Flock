const $ = (id) => document.getElementById(id);
const status = $('status');
const savedChannel = new URLSearchParams(location.search).get('channel') ?? '';
const channel = /^[1-9][0-9]*$/.test(savedChannel) ? savedChannel : '';
let busy = false;
let refreshing = false;
let actionRevision = 0;
let question;
let rendered = '';
let recentRendered = '';
const workAnswerDrafts = new Map();
const handoffLabels = {
  active: 'Active', paused: 'Paused in Flock',
  stopped: 'Stopped in Flock', completed: 'Completed', budget_exhausted: 'Budget exhausted',
};
const deliveryLabels = {
  prepared: 'Ready to send', dispatching: 'Sending', accepted: 'Accepted by T3', queued: 'Queued by T3',
  observed: 'Response observed', failed: 'Failed', rejected: 'Rejected', unknown: 'Delivery unknown',
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

function listItem(text) {
  const item = document.createElement('li');
  item.textContent = text;
  return item;
}

function workState(value) {
  return String(value ?? '').replaceAll('_', ' ');
}

function workAnswerKey(view, work, stage, prompt) {
  return JSON.stringify([view.channelId, work.delegationId, stage, prompt.waitId]);
}

function workAnswer(view, work, stage, prompt) {
  const key = workAnswerKey(view, work, stage, prompt);
  const form = document.createElement('form');
  const label = document.createElement('label');
  label.textContent = prompt.text;
  const input = document.createElement('textarea');
  input.rows = 2;
  input.required = true;
  input.maxLength = 8000;
  input.autocomplete = 'off';
  input.value = workAnswerDrafts.get(key) ?? '';
  input.addEventListener('input', () => workAnswerDrafts.set(key, input.value));
  label.append(input);
  const button = document.createElement('button');
  button.textContent = 'Send answer';
  form.append(label, button);
  form.onsubmit = (event) => {
    event.preventDefault();
    workAnswerDrafts.set(key, input.value);
    void run(async () => {
      const view = await request(`/${channel}/work-answer`, {
        generation: work.generation, delegationId: work.delegationId, stageId: stage,
        waitId: prompt.waitId, text: input.value,
      });
      workAnswerDrafts.delete(key);
      render(view);
      status.textContent = 'Answer recorded.';
    }, 'Could not record this work answer. Refresh the handoff and try again.');
  };
  return form;
}

function render(view) {
  const key = JSON.stringify(view);
  if (key === rendered) return;
  rendered = key;
  const activeWorkAnswers = new Set();
  for (const work of view.work) {
    if (work.question) activeWorkAnswers.add(workAnswerKey(view, work, null, work.question));
    for (const stage of work.stages) {
      if (stage.question) activeWorkAnswers.add(workAnswerKey(view, work, stage.stageId, stage.question));
    }
  }
  for (const draft of workAnswerDrafts.keys()) {
    if (!activeWorkAnswers.has(draft)) workAnswerDrafts.delete(draft);
  }
  const coordination = view.mode === 'coordinate';
  const stopped = view.frozen || ['completed', 'budget_exhausted'].includes(view.state);
  $('handoff').hidden = false;
  $('handoff-heading').textContent = `${coordination ? 'Coordination' : 'Consultation'} with ${view.botId}`;
  $('state').textContent = handoffLabels[view.state] ?? workState(view.state);
  fields('origin', [
    ['Requested by', view.person], ['Environment', view.target.environmentId], ['Project', view.target.projectId],
    ['Thread', view.target.threadId], ['Request', view.requestId],
    ...(coordination ? [['Goal', view.goal],
      ['Local generation', view.generation], ['Progress cursor', view.eventCursor]] : []),
  ]);
  const commandCount = view.residualCommandIds.length;
  const residualCommands = new Set(view.residualCommandIds);
  $('residual').hidden = commandCount === 0;
  $('residual-count').textContent = `${commandCount} in-flight or unresolved T3 ${commandCount === 1 ? 'message' : 'messages'}`;
  $('residual-commands').replaceChildren(...view.residualCommandIds.map(listItem));
  const childCount = view.admittedChildIds.length;
  $('residual-work').hidden = !view.frozen || childCount === 0;
  $('residual-work-count').textContent = `${childCount} child ${childCount === 1 ? 'launch' : 'launches'} admitted before pause or stop`;
  $('residual-children').replaceChildren(...view.admittedChildIds.map(listItem));

  $('no-deliveries').hidden = view.deliveries.length !== 0;
  $('deliveries').replaceChildren(...view.deliveries.map((delivery) => {
    const detail = document.createElement('details');
    const summary = document.createElement('summary');
    summary.textContent = [
      deliveryLabels[delivery.state] ?? workState(delivery.state),
      delivery.receipt?.mode,
      residualCommands.has(delivery.commandId) ? 'Outstanding after pause or stop' : '',
    ].filter(Boolean).join(' · ');
    const entries = [`Command ${delivery.commandId}`];
    if (delivery.receipt) entries.push(`Accepted Run ${delivery.receipt.runId}`, `Message ${delivery.receipt.messageId}`);
    if (delivery.observedRunId) entries.push(`Response observed in Run ${delivery.observedRunId}`);
    const list = document.createElement('ul');
    list.replaceChildren(...entries.map(listItem));
    detail.append(summary, list);
    const row = document.createElement('li');
    row.append(detail);
    return row;
  }));

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
    const showQuestion = (parent, stage, question, canAnswer) => {
      if (!question) return;
      if (!stopped && canAnswer) parent.append(workAnswer(view, work, stage, question));
      else {
        const text = document.createElement('p');
        text.textContent = question.text;
        parent.append(text);
      }
    };
    const notes = [work.purpose, `Requested by: ${workers.get(work.parentDelegationId) ?? view.botId}`,
      `Required evidence: ${work.doneWhen}`, `Input commit: ${work.commit}`,
      ...(work.result ? [work.result.report, ...work.result.evidence.map((source) =>
        [source.stageId, source.verdict, source.findings, source.threadId, source.runId, source.itemId]
          .filter(Boolean).join(' · '))] : []),
    ];
    content.replaceChildren(...notes.map(listItem));
    for (const stage of work.stages) {
      const row = listItem(`${stage.name} · ${workState(stage.status)}`);
      showQuestion(row, stage.stageId, stage.question, stage.canAnswer);
      content.append(row);
    }
    summary.append(title, content);
    showQuestion(summary, null, work.question, work.canAnswer);
    item.append(summary);
    return item;
  }));
  $('decision').hidden = !view.latestDecision;
  $('decision-text').textContent = view.latestDecision?.text ?? '';
  $('pause').hidden = !coordination || stopped || view.state === 'paused';
  $('resume').hidden = !coordination || view.state !== 'paused';
  $('stop').hidden = !coordination || ['stopped', 'completed', 'budget_exhausted'].includes(view.state);
  const nextQuestion = !stopped && view.pendingQuestion;
  if (question?.eventId !== nextQuestion?.eventId) $('answer').elements.text.value = '';
  question = nextQuestion ? { generation: view.generation, eventId: nextQuestion.eventId } : undefined;
  $('answer').hidden = !question;
  $('question').textContent = nextQuestion?.text ?? '';
}

async function run(work, message) {
  if (busy) return;
  busy = true;
  actionRevision += 1;
  const buttons = [...document.querySelectorAll('button:not(:disabled)')];
  for (const button of buttons) button.disabled = true;
  status.textContent = '';
  try { await work(); }
  catch { status.textContent = message; }
  finally {
    busy = false;
    for (const button of buttons) button.disabled = false;
  }
}

function renderRecent(recent) {
  const key = JSON.stringify(recent);
  if (key === recentRendered) return;
  recentRendered = key;
  $('recent').hidden = recent.length === 0;
  $('recent-links').replaceChildren(...recent.map((item) => {
    const row = document.createElement('li');
    const link = document.createElement('a');
    link.href = '/session-handoff.html?channel=' + item.channelId;
    link.textContent = `${item.mode === 'coordinate' ? 'Coordination' : 'Consultation'} with ${item.botId} · ${handoffLabels[item.state] ?? workState(item.state)}`;
    link.title = item.threadId;
    if (String(item.channelId) === channel) link.setAttribute('aria-current', 'page');
    row.append(link);
    return row;
  }));
}

async function loadRecent() {
  renderRecent(await request(''));
}

async function pollHandoffs() {
  if (busy || refreshing || document.hidden) return;
  refreshing = true;
  const revision = actionRevision;
  try {
    const recent = await request('');
    if (busy || revision !== actionRevision) return;
    renderRecent(recent);
    if (channel && !$('handoff').querySelector('form :focus')) {
      const view = await request(`/${channel}`);
      if (!busy && revision === actionRevision && !$('handoff').querySelector('form :focus')) render(view);
    }
  } catch {
  } finally { refreshing = false; }
}

$('refresh').addEventListener('click', () => void run(async () => {
  render(await request(`/${channel}`));
  await loadRecent();
}, 'Could not refresh the handoff.'));

for (const [id, action] of [['pause', 'pause'], ['resume', 'resume'], ['stop', 'freeze']]) {
  $(id).addEventListener('click', () => void run(async () => {
    render(await request(`/${channel}/${action}`, {}));
    await loadRecent();
  }, 'Could not update the handoff. Refresh and try again.'));
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
  }, 'Could not record the answer. Refresh and try again.');
});

void (async () => {
  await loadRecent();
  if (channel) render(await request(`/${channel}`));
})().catch(() => { status.textContent = 'Session handoffs are unavailable. Check your Flock sign-in.'; });
setInterval(() => void pollHandoffs(), 3500);
