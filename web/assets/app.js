import { showAvatar } from '/avatars.js';

const byId = (id) => document.getElementById(id);
const elements = Object.fromEntries([
  'navigation', 'pinned', 'bot-list', 'wait-list', 'wait-count', 'session-list', 'more-sessions',
  'connection', 'connection-text', 'open-navigation', 'close-navigation', 'profile', 'profile-avatar',
  'profile-name', 'profile-status', 'profile-placeholder', 'settings-button', 'conversation', 'older',
  'timeline', 'composer-container', 'composer', 'reply-context', 'reply-question', 'cancel-reply',
  'compose-label', 'message', 'send', 'composer-mode', 'notice', 'detail', 'detail-title', 'detail-body', 'close-detail',
  'theme-switch',
].map((id) => [id, byId(id)]));
const workspace = document.querySelector('.workspace');
const narrow = matchMedia('(max-width: 700px)');
const clock = new Intl.DateTimeFormat('ja-JP', { hour: '2-digit', minute: '2-digit' });
const dayFormat = new Intl.DateTimeFormat('ja-JP', { year: 'numeric', month: 'long', day: 'numeric' });
const fullTime = new Intl.DateTimeFormat('ja-JP', { dateStyle: 'medium', timeStyle: 'short' });
const byteLength = (text) => new TextEncoder().encode(text).length;
const stateLabels = {
  queued: '受付済み', deciding: '判定中', sending: '送信中', running: '作業中', waiting: '確認待ち',
  cancelling: '中止処理中', completed: '完了', cancelled: '中止', ended: '実行終了',
  idle: '待機中', thinking: '判断中', working: 'セッション作業中',
};
const waitLabels = {
  user_question: ['Bot からの質問', 'Bot が回答を待っています。'],
  budget: ['追加作業の許可', '追加作業の上限に達しました。1 回分の追加を許可できます。'],
  model_failure: ['再判定の確認', 'モデルの判定が複数回失敗しました。再判定を開始できます。'],
  t3_input: ['T3 での操作', '担当する Thread を開き、T3 上で承認または回答してください。'],
  deadline: ['期限の延長', '担当セッションが期限に達しました。新しい期限を指定できます。'],
  unknown_delivery: ['受付結果の再照合', 'T3 が作業または回答を受け付けたか確認できませんでした。'],
  auth: ['T3 の認証', 'T3 で再認証した後、再照合してください。'],
  settings: ['T3 の実行設定', 'T3 の接続や実行設定を確認した後、再照合してください。'],
};
const closedLabels = {
  answer: '回答済み', run_finished: 'セッションが終了しました', cancel: 'タスクを中止しました',
  session_question: 'セッションで確認内容が更新されました',
};
const errors = {
  invalid_text: '入力内容と文字数を確認してください。', invalid_deadline: '現在より後の期限を指定してください。',
  intake_settings: '依頼を受け付けられません。Bot の実行設定を確認してください。',
  operation_conflict: 'この操作はすでに処理されたか、確認内容が変わりました。',
  bot_not_found: 'Bot が見つかりません。',
};

function saved(key, fallback) {
  try { return JSON.parse(localStorage.getItem(key)) ?? fallback; }
  catch { return fallback; }
}
function save(key, value) {
  try { localStorage.setItem(key, JSON.stringify(value)); }
  catch { /* The session can continue with in-memory preferences. */ }
}
const savedPins = saved('flock-pins', []);
const pins = new Set(Array.isArray(savedPins) ? savedPins : []);
const drafts = new Map();
const replies = new Map();
const pendingRequests = new Set();
const pendingWaits = new Set();
const notices = new Map();
const signatures = new Map();
let bots = [];
let listLoaded = false;
let listGeneration = 0;
let view = null;
let menu = null;
let detail = null;
let profileAvatar = null;
let profileKey = '';
let connection = 'loading';

function node(tag, className, text) {
  const el = document.createElement(tag);
  if (className) el.className = className;
  if (text != null) el.textContent = text;
  return el;
}
function button(text, className, click) {
  const el = node('button', className, text);
  el.type = 'button';
  if (click) el.addEventListener('click', click);
  return el;
}
function avatar(bot, className = '') {
  const host = node('span', `avatar ${className}`.trim());
  showAvatar(host, bot);
  return host;
}
function linkToRun(run, label = 'T3 で開く') {
  if (!run?.link?.startsWith('https://app.t3.codes/threads/')) return null;
  const link = node('a', 'link', `${label} ↗`);
  link.href = run.link;
  link.target = '_blank';
  link.rel = 'noopener noreferrer';
  return link;
}
function waitLabel(reason) { return waitLabels[reason]?.[0] ?? '確認待ち'; }
function waitText(wait) { return wait.text || waitLabels[wait.reason]?.[1] || '確認内容を更新してください。'; }
function badge(state) { return node('span', `badge ${state}`, stateLabels[state] ?? state); }
function changed(key, ...values) {
  const signature = JSON.stringify(values);
  if (signatures.get(key) === signature) return false;
  signatures.set(key, signature);
  return true;
}
async function api(path, body) {
  const init = { credentials: 'same-origin' };
  if (body) Object.assign(init, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Flock-Action': '1' },
    body: JSON.stringify(body),
  });
  const response = await fetch(`/api/${path}`, init);
  const data = await response.json();
  if (!response.ok) throw Object.assign(new Error(data.error), { status: response.status });
  return data;
}
const botQuery = (path, bot, before) => `${path}?${new URLSearchParams(before ? { bot, before } : { bot })}`;

function setConnection(state) {
  if (connection === state) return;
  connection = state;
  elements.connection.dataset.state = state;
  elements['connection-text'].textContent = state === 'offline' ? 'Runtime に接続できません' : '';
  renderHeader();
}
function renderTheme(mode) {
  for (const option of elements['theme-switch'].querySelectorAll('button')) {
    option.setAttribute('aria-pressed', option.dataset.themeMode === mode);
  }
}
function setTheme(mode) {
  save('flock-theme', mode);
  document.dispatchEvent(new CustomEvent('flock-theme', { detail: mode }));
  renderTheme(mode);
}
function openNavigation(open, focus = true) {
  const expanded = open && narrow.matches;
  workspace.classList.toggle('nav-open', expanded);
  elements['open-navigation'].setAttribute('aria-expanded', expanded);
  if (focus && narrow.matches) elements[expanded ? 'close-navigation' : 'open-navigation'].focus();
}
function selectBot(botId) {
  menu = null;
  openNavigation(false);
  if (view?.id === botId) { renderBots(); return; }
  closeDetail();
  view = botId ? { id: botId, data: null, events: new Map(), olderBefore: 0, loadedSessions: new Map(),
    sessionsBefore: 0, loadingOlder: false, loadingSessions: false, generation: 0 } : null;
  save('flock-selected-bot', botId);
  render();
  elements.conversation.scrollTop = 0;
  if (view) void refreshView(view);
}

function renderBots() {
  if (!changed('bots', bots, [...pins], menu, view?.id, listLoaded)) return;
  const pinned = [];
  const rest = [];
  for (const bot of bots) {
    const isPinned = pins.has(bot.id);
    const row = node('div', isPinned ? 'pinned-bot' : 'bot-row');
    const active = view?.id === bot.id;
    row.classList.toggle('selected', active);
    const select = button(null, isPinned ? 'pin' : 'bot-select', () => selectBot(bot.id));
    select.title = bot.name;
    select.setAttribute('aria-label', bot.name);
    if (active) select.setAttribute('aria-current', 'true');
    select.append(avatar(bot));
    if (!isPinned) {
      const text = node('span', 'bot-text');
      text.append(node('span', 'bot-name', bot.name), node('span', 'bot-preview', bot.preview || '会話はまだありません'));
      select.append(text);
    }
    const toggle = button('⋯', 'icon-button pin-menu', () => {
      const opened = menu !== bot.id;
      menu = opened ? bot.id : null;
      renderBots();
      const replacement = [...document.querySelectorAll('.pin-menu')].find((el) => el.dataset.bot === bot.id);
      if (opened) replacement?.nextElementSibling?.querySelector('button')?.focus();
      else replacement?.focus();
    });
    toggle.dataset.bot = bot.id;
    toggle.setAttribute('aria-label', `${bot.name} のメニュー`);
    toggle.setAttribute('aria-expanded', menu === bot.id);
    row.append(select, toggle);
    if (menu === bot.id) {
      const popover = node('div', 'pin-popover');
      popover.append(button(isPinned ? 'ピン留めを解除' : 'ピン留めする', null, () => {
        if (isPinned) pins.delete(bot.id);
        else pins.add(bot.id);
        save('flock-pins', [...pins]);
        menu = null;
        renderBots();
        [...document.querySelectorAll('.pin-menu')].find((el) => el.dataset.bot === bot.id)?.focus();
      }));
      row.append(popover);
    }
    (isPinned ? pinned : rest).push(row);
  }
  elements.pinned.replaceChildren(...pinned);
  elements.pinned.hidden = !pinned.length;
  elements['bot-list'].replaceChildren(...rest);
  if (!bots.length) elements['bot-list'].append(node('p', 'activity-empty', listLoaded ? '設定済みの Bot はありません' : '読み込み中…'));
}

function renderHeader() {
  const bot = view?.data?.bot ?? bots.find((item) => item.id === view?.id);
  elements.profile.hidden = !bot;
  elements['settings-button'].hidden = !bot;
  elements['profile-placeholder'].hidden = !!bot;
  if (!bot) {
    profileAvatar?.destroy();
    profileAvatar = null;
    profileKey = '';
    return;
  }
  const presence = view?.data?.presence ?? 'idle';
  const key = JSON.stringify([bot.id, bot.avatar]);
  if (key !== profileKey) {
    profileAvatar?.destroy();
    profileAvatar = showAvatar(elements['profile-avatar'], bot, { still: false, state: presence });
    profileKey = key;
  } else if (elements['profile-status'].dataset.state !== presence) profileAvatar.update({ state: presence });
  elements['profile-name'].textContent = bot.name;
  elements.profile.setAttribute('aria-label', `${bot.name} の設定を開く`);
  elements['profile-status'].dataset.state = presence;
  elements['profile-status'].textContent = connection === 'offline' ? 'Runtime に未接続' : stateLabels[presence];
}

function activityTitle(request, className) {
  const title = node('span', className, request);
  title.title = request;
  return title;
}
function renderActivity() {
  const data = view?.data;
  const waits = data?.waits ?? [];
  if (changed('waits', view?.id, waits)) {
    elements['wait-count'].textContent = waits.length;
    elements['wait-count'].classList.toggle('has-waits', waits.length > 0);
    const rows = waits.map((task) => {
      const row = button(null, 'activity wait-select', () => {
        openNavigation(false);
        openDetail({ kind: 'wait', taskId: task.id, waitId: task.wait.id });
      });
      const line = node('span', 'activity-line');
      line.append(node('span', 'wait-dot'), node('span', 'activity-title', waitLabel(task.wait.reason)));
      row.append(line, activityTitle(task.request, 'activity-subtitle'));
      return row;
    });
    elements['wait-list'].replaceChildren(...rows);
    if (!rows.length) elements['wait-list'].append(node('p', 'activity-empty', '確認待ちはありません'));
  }
  const groups = new Map(view?.loadedSessions);
  for (const task of data?.sessionTasks ?? []) groups.set(task.id, task);
  const active = data?.activeSessions ?? [];
  const more = elements['more-sessions'];
  more.hidden = !view?.sessionsBefore;
  more.disabled = !!view?.loadingSessions;
  if (!changed('sessions', view?.id, [...groups.values()], active)) return;
  const rows = [];
  for (const task of [...groups.values()].sort((a, b) => b.id - a.id)) {
    for (const session of task.sessions) {
      const row = node('article', 'activity session-entry');
      const status = active.find((run) => run.taskId === task.id && run.threadId === session.threadId)?.state ?? 'ended';
      const line = node('span', 'activity-line');
      line.append(badge(status), node('span', 'activity-subtitle', `#${task.id}`));
      row.append(activityTitle(task.request, 'activity-title'), line);
      const link = linkToRun(session);
      if (link) row.append(link);
      rows.push(row);
    }
  }
  elements['session-list'].replaceChildren(...rows);
  if (!rows.length) elements['session-list'].append(node('p', 'activity-empty', 'セッションはまだありません'));
}

function beginReply(task) {
  const current = replies.get(view.id);
  if (current?.waitId !== task.wait.id) replies.set(view.id, {
    taskId: task.id, waitId: task.wait.id, question: waitText(task.wait), text: '',
  });
  closeDetail();
  renderComposer();
  elements.message.focus();
}
function questionActions(task) {
  const actions = node('div', 'event-actions');
  for (const choice of task.wait.choices ?? []) {
    const option = button(choice, 'button', () => void operate(view, task, 'answer', { text: choice }));
    option.disabled = pendingWaits.has(task.wait.id);
    actions.append(option);
  }
  const reply = button('返信を書く', 'text-button', () => beginReply(task));
  reply.disabled = pendingWaits.has(task.wait.id);
  actions.append(reply);
  return actions;
}
function detailText(label, value) {
  const row = node('p', 'event-detail');
  row.append(node('strong', null, label), document.createTextNode(value));
  return row;
}
function eventCard(event, waits, closed, queued) {
  const data = event.data;
  const outbound = event.kind === 'user_answer' || (event.kind === 'request' && data.source !== 'cron');
  const row = node('article', `event${outbound ? ' outbound' : ''}`);
  const card = node('div', 'event-card');
  const metadata = node('div', 'event-meta');
  const timestamp = new Date(event.time * 1000);
  const time = node('time', null, clock.format(timestamp));
  time.dateTime = timestamp.toISOString();
  const labels = { request: data.source === 'cron' ? '定期依頼' : 'あなた', user_answer: 'あなたの回答',
    report: 'セッション報告', completed: '完了報告', cancelled: 'タスクを中止' };
  const label = event.kind === 'waiting' ? waitLabel(data.reason) : labels[event.kind];
  metadata.append(node('strong', null, label), node('span', null, `#${event.taskId}`), time);
  row.append(metadata, card);
  if (event.kind === 'waiting') {
    card.classList.add('question-card');
    card.append(node('p', 'event-text', waitText(data)));
    if (data.decisionReason) card.append(detailText('判断理由', data.decisionReason));
    const task = waits.get(data.id);
    if (task?.wait.reason === 'user_question') card.append(questionActions(task));
    else if (task) {
      const actions = node('div', 'event-actions');
      actions.append(button('確認内容を開く', 'button', () => openDetail({ kind: 'wait', taskId: task.id, waitId: data.id })));
      card.append(actions);
    } else card.append(node('p', 'event-note', closedLabels[closed.get(data.id)] ?? '確認待ちは終了しました'));
  } else if (event.kind === 'report') {
    card.append(node('p', 'event-text', data.report || 'セッションから報告がありました'));
    if (data.status) card.append(detailText('Run の状態', stateLabels[data.status] ?? data.status));
    if (data.truncated) card.append(node('p', 'event-note', '報告の続きは T3 で確認できます。'));
    const link = linkToRun(data.run);
    if (link) {
      const actions = node('div', 'event-actions');
      actions.append(link);
      card.append(actions);
    }
  } else if (event.kind === 'completed') {
    card.classList.add('finished-card');
    card.append(node('p', 'event-text', data.text));
    if (data.reason) card.append(detailText('判断理由', data.reason));
  } else if (event.kind === 'cancelled') card.append(node('p', 'event-text', 'タスクを中止しました。'));
  else {
    card.append(node('p', 'event-text', data.text));
    if (data.source === 'cron' && data.scheduledAt) card.append(node('p', 'event-note', `予定時刻 ${fullTime.format(data.scheduledAt * 1000)}`));
    const blocker = queued.get(event.taskId)?.blockedBy;
    if (event.kind === 'request' && blocker) {
      const name = bots.find((bot) => bot.id === blocker.botId)?.name ?? blocker.botId;
      card.append(node('p', 'event-note', `先行タスク #${blocker.id}（${name}）の完了を待っています。`));
      card.append(node('p', 'event-note', blocker.request));
    }
  }
  return row;
}

function renderTimeline(scroll = 'auto') {
  const older = elements.older;
  const data = view?.data;
  const events = [...(view?.events.values() ?? [])].sort((a, b) => a.id - b.id);
  older.hidden = !view?.olderBefore;
  older.disabled = !!view?.loadingOlder;
  if (!changed('timeline', view?.id, data?.bot, events, data?.waits, data?.queued, [...pendingWaits], listLoaded)) return;
  const scroller = elements.conversation;
  const previousHeight = scroller.scrollHeight;
  const previousTop = scroller.scrollTop;
  const atBottom = previousHeight - previousTop - scroller.clientHeight < 65;
  const waits = new Map((data?.waits ?? []).map((task) => [task.wait.id, task]));
  const queued = new Map((data?.queued ?? []).map((task) => [task.id, task]));
  const closed = new Map(events.filter((event) => event.kind === 'wait_closed').map((event) => [event.data.waitId, event.data.resolution]));
  const fragment = document.createDocumentFragment();
  let day = '';
  for (const event of events) {
    if (event.kind === 'wait_closed') continue;
    const eventDay = dayFormat.format(event.time * 1000);
    if (eventDay !== day) {
      fragment.append(node('p', 'day', eventDay));
      day = eventDay;
    }
    fragment.append(eventCard(event, waits, closed, queued));
  }
  if (!fragment.childElementCount) {
    const empty = node('div', 'empty-state');
    if (data?.bot) empty.append(avatar(data.bot));
    const loaded = !!data || (!view && listLoaded);
    empty.append(node('h2', null, loaded ? (data ? `${data.bot.name} との会話` : 'Bot がありません') : '読み込み中…'));
    if (data) empty.append(node('p', null, '新しい依頼を送ると、ここに履歴が表示されます。'));
    fragment.append(empty);
  }
  elements.timeline.replaceChildren(fragment);
  if (scroll === 'older') scroller.scrollTop = previousTop + scroller.scrollHeight - previousHeight;
  else if (scroll === 'bottom' || atBottom) scroller.scrollTop = scroller.scrollHeight;
  else scroller.scrollTop = previousTop;
}

const sendKey = /mac|iphone|ipad/i.test(navigator.userAgentData?.platform ?? navigator.platform) ? '⌘' : 'Ctrl';
function setNotice(context, text, isError = false) {
  notices.set(context.id, { text, isError });
  if (view?.id === context.id) renderComposer();
}
function scheduleNoticeFade(context, notice) {
  if (!notice || notice.isError || notice.timer || pendingRequests.has(context.id) || pendingWaits.size) return;
  notice.timer = setTimeout(() => {
    notice.timer = null;
    if (notices.get(context.id) !== notice || view?.id !== context.id) return;
    notice.fading = true;
    renderComposer();
    setTimeout(() => {
      if (notices.get(context.id) !== notice) return;
      notices.delete(context.id);
      if (view?.id === context.id) renderComposer();
    }, 400);
  }, 3600);
}
function fitMessage() {
  const field = elements.message;
  if (!field.offsetParent) return;
  field.style.height = 'auto';
  field.style.height = `${field.scrollHeight}px`;
}
function renderComposer() {
  const context = view;
  elements['composer-container'].hidden = !context?.data;
  if (!context?.data) return;
  const reply = replies.get(context.id);
  const notice = notices.get(context.id);
  const body = reply ? reply.text : drafts.get(context.id) ?? '';
  const busy = pendingRequests.has(context.id) || (!!reply && pendingWaits.has(reply.waitId));
  elements['reply-context'].hidden = !reply;
  elements['reply-question'].textContent = reply?.question ?? '';
  elements['reply-context'].title = reply ? `#${reply.taskId} ${reply.question}` : '';
  elements['compose-label'].textContent = reply ? '質問への返信' : '新しい依頼';
  elements['cancel-reply'].disabled = busy;
  elements.message.placeholder = reply ? '回答を入力' : `${context.data.bot.name} に依頼`;
  if (elements.message.value !== body) {
    elements.message.value = body;
    fitMessage();
  }
  elements.message.readOnly = busy;
  elements.composer.setAttribute('aria-busy', String(busy));
  elements.send.disabled = busy || !body.trim() || byteLength(body) > (reply ? 8000 : 16000);
  elements.composer.classList.toggle('can-send', !elements.send.disabled);
  elements.send.setAttribute('aria-label', reply ? '質問に返信する' : '新しい依頼を送る');
  elements.send.title = `${reply ? '返信する' : '送信'} (${sendKey}+Enter)`;
  elements.notice.textContent = notice?.text ?? '';
  elements.notice.classList.toggle('error', !!notice?.isError);
  elements.notice.classList.toggle('fading', !!notice?.fading);
  scheduleNoticeFade(context, notice);
}

function closeDetail() {
  detail = null;
  if (elements.detail.open) elements.detail.close();
}
function openDetail(selection) {
  if (!view?.data) return;
  detail = selection;
  signatures.delete('detail');
  renderDetail();
  if (!elements.detail.open) elements.detail.showModal();
}
function settingsContent(bot) {
  elements['detail-title'].textContent = 'Bot 設定';
  const profile = node('div', 'dialog-profile');
  profile.append(avatar(bot), node('span', null, bot.name));
  const role = node('section', 'dialog-section');
  role.append(node('h3', null, '役割'), node('p', 'prose', bot.soul || '役割は設定されていません。'));
  const schedule = node('section', 'dialog-section');
  schedule.append(node('h3', null, '定期依頼'));
  for (const item of bot.cron) {
    const row = node('div', 'schedule-row');
    const specification = [item.schedule ?? item.expression ?? item.cron, item.timezone ?? item.timeZone, item.id,
      item.enabled === false ? '無効' : ''].filter(Boolean).join(' · ');
    row.append(node('p', 'prose', item.request), node('p', 'dialog-meta', specification));
    schedule.append(row);
  }
  if (!bot.cron.length) schedule.append(node('p', 'prose', '定期依頼はありません。'));
  return [profile, role, schedule];
}
function waitForm(label, fields, submit) {
  const form = node('form', 'wait-form');
  for (const field of fields) {
    const wrapper = node('label', null, field.label);
    const input = node('input');
    Object.assign(input, { name: field.name, type: field.type ?? 'text', value: field.value ?? '', required: field.required ?? false });
    wrapper.append(input);
    form.append(wrapper);
  }
  const action = button(label, 'button');
  action.type = 'submit';
  form.append(action);
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    submit(Object.fromEntries(new FormData(form)));
  });
  return form;
}
function localDateTime(timestamp) {
  const date = new Date(timestamp);
  return new Date(timestamp - date.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
}
function waitContent(context, task) {
  const wait = task.wait;
  elements['detail-title'].textContent = waitLabel(wait.reason);
  const request = node('section', 'dialog-section');
  request.append(node('h3', null, `依頼 #${task.id}`), node('p', 'dialog-request', task.request));
  const reason = node('section', 'dialog-section');
  reason.append(node('h3', null, '待っている理由'), node('p', 'prose', waitText(wait)));
  if (wait.decisionReason) reason.append(detailText('判断理由', wait.decisionReason));
  const actions = node('section', 'dialog-section');
  actions.append(node('h3', null, 'できる操作'));
  const buttons = node('div', 'event-actions');
  const runLink = linkToRun(wait.run, wait.reason === 't3_input' ? 'T3 で承認する' : 'T3 で開く');
  if (runLink) buttons.append(runLink);
  const perform = (operation, fields) => void operate(context, task, operation, fields);
  if (wait.reason === 'user_question') actions.append(questionActions(task));
  else if (wait.reason === 'budget') buttons.append(button('追加 1 回を許可', 'button', () => perform('approve')));
  else if (wait.reason === 'model_failure') buttons.append(button('再判定', 'button', () => perform('retry')));
  else if (wait.reason === 'deadline') {
    if (task.deadline) reason.append(node('p', 'dialog-meta', `現在の期限 ${fullTime.format(task.deadline * 1000)}`));
    actions.append(waitForm('期限を延長', [{ name: 'deadline', label: '新しい期限（端末の時刻）',
      type: 'datetime-local', value: localDateTime(Date.now() + 60 * 60 * 1000), required: true }], ({ deadline }) => {
      const next = Math.floor(new Date(deadline).getTime() / 1000);
      if (next > Date.now() / 1000) perform('extend', { deadline: next });
      else showDetailError(errors.invalid_deadline);
    }));
  } else if (['unknown_delivery', 'auth', 'settings'].includes(wait.reason)) {
    buttons.append(button('再照合', 'button', () => perform('reconcile')));
    if (wait.reason === 'unknown_delivery') {
      const advanced = node('details');
      advanced.append(node('summary', 'dialog-meta', '担当する Run を指定'), waitForm('指定した Run と照合', [
        { name: 'threadId', label: 'Thread ID', required: true }, { name: 'runId', label: 'Run ID', required: true },
        { name: 'questionId', label: '質問 ID（任意）' },
      ], (fields) => perform('reconcile', Object.fromEntries(Object.entries(fields).filter(([, value]) => value.trim())))));
      actions.append(advanced);
    }
  }
  buttons.append(button('タスクを中止', 'button danger', () => {
    if (window.confirm(`タスク #${task.id} を中止しますか？`)) perform('cancel');
  }));
  actions.append(buttons);
  const feedback = node('p', 'dialog-feedback');
  feedback.id = 'detail-feedback';
  feedback.setAttribute('role', 'status');
  return [request, reason, actions, feedback];
}
function showDetailError(text) {
  const output = byId('detail-feedback');
  if (output) output.textContent = text;
}
function renderDetail() {
  if (!detail || !view?.data) return;
  if (detail.kind === 'settings') {
    const bot = view.data.bot;
    if (changed('detail', detail, bot)) elements['detail-body'].replaceChildren(...settingsContent(bot));
    return;
  }
  const task = view.data.waits.find((item) => item.id === detail.taskId && item.wait.id === detail.waitId);
  if (!task) {
    if (changed('detail', detail, null)) {
      elements['detail-title'].textContent = '確認待ちは終了しました';
      elements['detail-body'].replaceChildren(node('p', 'prose', '最新の内容は会話で確認できます。'));
    }
    return;
  }
  if (changed('detail', detail, task)) elements['detail-body'].replaceChildren(...waitContent(view, task));
  for (const control of elements['detail-body'].querySelectorAll('button, input')) control.disabled = pendingWaits.has(task.wait.id);
}
function render(scroll) {
  renderBots();
  renderHeader();
  renderActivity();
  renderTimeline(scroll);
  renderComposer();
  renderDetail();
}

function mergeLatest(context, latest) {
  const { events, olderBefore } = latest;
  const first = events[0]?.id;
  // A long disconnection can leave a gap larger than the newest page.
  // Re-anchor pagination at the newest page so the next older page fills it.
  if (!context.events.size || (first && first > context.newestEvent)) {
    context.events.clear();
    context.olderBefore = olderBefore;
  }
  for (const event of events) context.events.set(event.id, event);
  context.newestEvent = events.at(-1)?.id ?? context.newestEvent;
  if (!context.loadedSessions.size) context.sessionsBefore = latest.sessionsBefore;
  context.data = latest;
  const reply = replies.get(context.id);
  if (reply && !latest.waits.some((task) => task.id === reply.taskId && task.wait.id === reply.waitId)) {
    replies.delete(context.id);
    setNotice(context, '返信先の確認待ちは終了しました。');
  }
}
async function refreshView(context, scroll) {
  const generation = ++context.generation;
  try {
    const latest = await api(botQuery('view', context.id));
    if (context !== view || generation !== context.generation) return;
    mergeLatest(context, latest);
    setConnection('ready');
    render(scroll);
  } catch (error) {
    if (context !== view || generation !== context.generation) return;
    if (error.status === 404) void refreshBots();
    else setConnection('offline');
  }
}
async function refreshBots() {
  const generation = ++listGeneration;
  try {
    const latest = await api('bots');
    if (generation !== listGeneration) return;
    bots = latest;
    listLoaded = true;
    setConnection('ready');
    if (!bots.some((bot) => bot.id === view?.id)) {
      const preferred = saved('flock-selected-bot', null);
      selectBot(bots.find((bot) => bot.id === preferred)?.id ?? bots[0]?.id ?? null);
    }
    render();
  } catch {
    if (generation === listGeneration) setConnection('offline');
  }
}
async function loadOlder() {
  const context = view;
  const cursor = context?.olderBefore;
  if (!cursor || context.loadingOlder) return;
  context.loadingOlder = true;
  renderTimeline();
  try {
    const page = await api(botQuery('conversation', context.id, cursor));
    if (context !== view || context.olderBefore !== cursor) return;
    context.olderBefore = page.olderBefore;
    for (const event of page.events) context.events.set(event.id, event);
    renderTimeline('older');
  } catch { if (context === view) setNotice(context, '以前の会話を読み込めませんでした。', true); }
  finally {
    context.loadingOlder = false;
    if (context === view) renderTimeline();
  }
}
async function loadSessions() {
  const context = view;
  const cursor = context?.sessionsBefore;
  if (!cursor || context.loadingSessions) return;
  context.loadingSessions = true;
  renderActivity();
  try {
    const page = await api(botQuery('sessions', context.id, cursor));
    if (context !== view) return;
    for (const task of page.sessionTasks) context.loadedSessions.set(task.id, task);
    context.sessionsBefore = page.sessionsBefore;
  } catch { if (context === view) setNotice(context, '以前のセッションを読み込めませんでした。', true); }
  finally {
    context.loadingSessions = false;
    if (context === view) renderActivity();
  }
}

async function operate(context, task, operation, fields = {}) {
  const waitId = task.wait.id;
  if (!context || pendingWaits.has(waitId)) return;
  pendingWaits.add(waitId);
  setNotice(context, '操作を送信中…');
  if (view?.id === context.id) render();
  try {
    await api('operations', { botId: context.id, taskId: task.id, waitId, operation, ...fields });
    if (replies.get(context.id)?.waitId === waitId) replies.delete(context.id);
    setNotice(context, operation === 'answer' ? '回答を受け付けました。' : '操作を受け付けました。');
    if (view?.id === context.id && detail?.waitId === waitId) closeDetail();
  } catch (error) {
    if (error.status === 409 && replies.get(context.id)?.waitId === waitId) replies.delete(context.id);
    const message = errors[error.message] ?? '操作の結果を確認できません。会話を確認してください。';
    setNotice(context, message, true);
    if (view?.id === context.id && detail?.waitId === waitId) showDetailError(message);
  } finally {
    if (view?.id === context.id) await refreshView(view);
    pendingWaits.delete(waitId);
    if (view?.id === context.id) render();
  }
}
async function submit(event) {
  event.preventDefault();
  const context = view;
  if (!context?.data || elements.send.disabled) return;
  const reply = replies.get(context.id);
  if (reply) {
    const task = context.data.waits.find((item) => item.id === reply.taskId && item.wait.id === reply.waitId);
    if (task) await operate(context, task, 'answer', { text: reply.text });
    return;
  }
  const request = drafts.get(context.id);
  pendingRequests.add(context.id);
  setNotice(context, '依頼を送信中…');
  try {
    const { taskId } = await api('requests', { botId: context.id, request });
    drafts.delete(context.id);
    setNotice(context, `依頼 #${taskId} を受け付けました。`);
    if (view?.id === context.id) await refreshView(view, 'bottom');
    void refreshBots();
  } catch (error) { setNotice(context, errors[error.message] ?? '依頼の送信結果を確認できません。会話を確認してください。', true); }
  finally {
    pendingRequests.delete(context.id);
    if (view?.id === context.id) renderComposer();
  }
}

const savedTheme = saved('flock-theme', 'system');
renderTheme(['light', 'dark'].includes(savedTheme) ? savedTheme : 'system');
elements['theme-switch'].addEventListener('click', (event) => {
  const option = event.target.closest('button[data-theme-mode]');
  if (option) setTheme(option.dataset.themeMode);
});
elements['open-navigation'].addEventListener('click', () => openNavigation(true));
elements['close-navigation'].addEventListener('click', () => openNavigation(false));
narrow.addEventListener('change', () => openNavigation(false, false));
for (const id of ['profile', 'settings-button']) elements[id].addEventListener('click', () => openDetail({ kind: 'settings' }));
elements['close-detail'].addEventListener('click', closeDetail);
elements.detail.addEventListener('close', () => { if (!elements.detail.open) detail = null; });
elements.older.addEventListener('click', loadOlder);
elements['more-sessions'].addEventListener('click', loadSessions);
elements.composer.addEventListener('submit', submit);
elements['cancel-reply'].addEventListener('click', () => {
  replies.delete(view.id);
  renderComposer();
  elements.message.focus();
});
elements.message.addEventListener('input', () => {
  if (!view) return;
  const reply = replies.get(view.id);
  if (reply) reply.text = elements.message.value;
  else drafts.set(view.id, elements.message.value);
  fitMessage();
  notices.delete(view.id);
  const limit = reply ? 8000 : 16000;
  if (byteLength(elements.message.value) > limit) setNotice(view, `入力は ${limit.toLocaleString('ja-JP')} バイトまでです。`, true);
  else renderComposer();
});
// Safari reports the composition-committing Enter with keyCode 229 and isComposing false.
elements.composer.addEventListener('keydown', (event) => {
  if (event.isComposing || event.keyCode === 229) return;
  if (event.key === 'Enter' && (event.metaKey || event.ctrlKey) && event.target === elements.message) {
    event.preventDefault();
    elements.send.click();
  } else if (event.key === 'Escape' && !elements['reply-context'].hidden) {
    event.preventDefault();
    event.stopPropagation();
    elements['cancel-reply'].click();
  }
});
byId('send-hint').textContent = `${sendKey} ↵`;
let composerHeight = 0;
function resizeComposerAndKeepTimelineAtBottom() {
  const height = elements['composer-container'].offsetHeight;
  const composerGrowth = height - composerHeight;
  composerHeight = height;
  const scroller = elements.conversation;
  const distanceFromPreviousBottom = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight - composerGrowth;
  if (composerGrowth > 0 && distanceFromPreviousBottom <= 4) scroller.scrollTop = scroller.scrollHeight;
  fitMessage();
}
new ResizeObserver(resizeComposerAndKeepTimelineAtBottom).observe(elements['composer-container']);
document.addEventListener('click', (event) => {
  if (menu && !event.target.closest('.pin-menu, .pin-popover')) { menu = null; renderBots(); }
});
document.addEventListener('keydown', (event) => {
  if (event.key !== 'Escape') return;
  if (menu) {
    const bot = menu;
    menu = null;
    renderBots();
    [...document.querySelectorAll('.pin-menu')].find((el) => el.dataset.bot === bot)?.focus();
  } else if (workspace.classList.contains('nav-open') && !elements.detail.open) openNavigation(false);
});

function refresh() {
  if (document.hidden) return;
  void refreshBots();
  if (view) void refreshView(view);
}
render();
refresh();
setInterval(refresh, 2500);
document.addEventListener('visibilitychange', refresh);
window.addEventListener('pageshow', (event) => { if (event.persisted) refresh(); });
