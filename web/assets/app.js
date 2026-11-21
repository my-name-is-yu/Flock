import { showAvatar } from '/avatars.js';
import { accentColor } from '/avatars.js';

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
  // A healthy connection needs no label; only the first load and an outage are announced.
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

const presenceLabels = { idle: '待機中', thinking: '判断中', working: '作業中', waiting: '確認待ち' };
function renderHeader() {
  const bot = view?.data?.bot ?? bots.find((item) => item.id === view?.id);
  const profile = elements.profile;
  profile.hidden = !bot;
  elements['settings-button'].hidden = !bot;
  elements['profile-placeholder'].hidden = !!bot;
  if (!bot) {
    profileAvatar?.destroy();
    profileAvatar = null;
    profileKey = '';
    elements.conversation.style.removeProperty('--bot-accent');
    return;
  }
  const presence = view?.data?.presence ?? 'idle';
  const key = JSON.stringify([bot.id, bot.avatar]);
  if (key !== profileKey) {
    profileAvatar?.destroy();
    profileAvatar = showAvatar(elements['profile-avatar'], bot, { still: false, state: presence });
    profileKey = key;
    elements.conversation.style.setProperty('--bot-accent', accentColor(bot));
  } else if (profile.dataset.presence !== presence) profileAvatar?.update({ state: presence });
  const offline = connection === 'offline';
  const status = offline ? 'Runtime に未接続' : presenceLabels[presence] ?? presence;
  profile.dataset.presence = presence;
  profile.dataset.state = offline ? 'offline' : presence;
  profile.title = `${bot.name} · ${status}`;
  profile.setAttribute('aria-label', `${bot.name} の設定を開く`);
  elements['profile-name'].textContent = bot.name;
  elements['profile-status'].textContent = status;
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
const icons = {
  copy: '<path d="M9 9.5A1.5 1.5 0 0 1 10.5 8h7A1.5 1.5 0 0 1 19 9.5v9a1.5 1.5 0 0 1-1.5 1.5h-7A1.5 1.5 0 0 1 9 18.5z"/><path d="M15 8V5.5A1.5 1.5 0 0 0 13.5 4h-7A1.5 1.5 0 0 0 5 5.5v9A1.5 1.5 0 0 0 6.5 16H9"/>',
  copied: '<path d="m5.5 12.5 4 4 9-9"/>',
  reply: '<path d="M9.5 7 4.5 12l5 5"/><path d="M5 12h9a5.5 5.5 0 0 1 5.5 5.5V19"/>',
  check: '<circle cx="12" cy="12" r="8.5"/><path d="m8.5 12.3 2.4 2.4 4.6-4.9"/>',
  clock: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/>',
  open: '<path d="M10 5H6.5A1.5 1.5 0 0 0 5 6.5v11A1.5 1.5 0 0 0 6.5 19h11a1.5 1.5 0 0 0 1.5-1.5V14"/><path d="M14 5h5v5M19 5l-8 8"/>',
};
function icon(name, className = 'glyph') {
  const host = node('span', className);
  host.setAttribute('aria-hidden', 'true');
  host.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${icons[name]}</svg>`;
  return host;
}
function iconButton(name, label, click) {
  const control = button(null, 'icon-button msg-action', click);
  control.setAttribute('aria-label', label);
  control.title = label;
  control.append(icon(name));
  return control;
}
async function copyMessage(control, text) {
  try {
    await navigator.clipboard.writeText(text);
    control.replaceChildren(icon('copied'));
    control.setAttribute('aria-label', 'コピーしました');
    control.title = 'コピーしました';
    setTimeout(() => {
      control.replaceChildren(icon('copy'));
      control.setAttribute('aria-label', 'テキストをコピー');
      control.title = 'テキストをコピー';
    }, 1600);
  } catch { if (view) setNotice(view, 'コピーできませんでした。', true); }
}
function questionActions(task) {
  const actions = node('div', 'event-actions question-actions');
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
function messageSide(event) {
  if (event.kind === 'cancelled') return 'system';
  return event.kind === 'user_answer' || (event.kind === 'request' && event.data.source !== 'cron') ? 'user' : 'bot';
}
function reasonText(text) {
  const reason = node('p', 'event-reason');
  reason.append(node('span', 'sr-only', '判断理由: '), document.createTextNode(text));
  return reason;
}
function eventTime(event, extra) {
  const timestamp = new Date(event.time * 1000);
  const time = node('time', 'msg-time', clock.format(timestamp));
  time.dateTime = timestamp.toISOString();
  time.title = [`#${event.taskId}`, fullTime.format(timestamp), extra].filter(Boolean).join(' · ');
  return time;
}
function eventCard(event, context, place) {
  const data = event.data;
  const bot = context.bot;
  const side = messageSide(event);
  if (side === 'system') {
    const note = node('p', 'system-note', `#${event.taskId} を中止しました`);
    note.title = fullTime.format(event.time * 1000);
    return note;
  }
  const row = node('article', `msg ${side}`);
  row.classList.toggle('group-start', place.groupStart);
  row.classList.toggle('task-start', place.taskStart);
  const cron = event.kind === 'request' && data.source === 'cron';
  const scheduled = cron && data.scheduledAt ? `予定 ${fullTime.format(data.scheduledAt * 1000)}` : '';
  const showMeta = side === 'bot' ? place.groupStart || place.taskStart : place.taskStart;
  const meta = node('div', showMeta ? 'msg-meta' : 'msg-meta sr-only');
  const name = node('span', side === 'bot' && place.groupStart ? 'msg-name' : 'msg-name sr-only',
    side === 'bot' ? bot?.name ?? 'Bot' : 'あなた');
  const task = node('span', place.taskStart ? 'msg-task' : 'msg-task sr-only', `#${event.taskId}`);
  meta.append(name, task, eventTime(event, scheduled));
  row.append(meta);

  const line = node('div', 'msg-line');
  if (side === 'bot') line.append(place.groupEnd && bot ? avatar(bot, 'msg-avatar') : node('span', 'msg-avatar-space'));
  const bubble = node('div', 'bubble');
  const text = node('p', 'event-text');
  const actions = node('div', 'msg-actions');
  if (!showMeta) {
    const hoverTime = eventTime(event, scheduled);
    hoverTime.setAttribute('aria-hidden', 'true');
    actions.append(hoverTime);
  }
  const extras = [];
  let copy = data.text ?? '';
  const openTask = event.kind === 'waiting' ? context.waits.get(data.id) : null;

  if (event.kind === 'waiting') {
    copy = waitText(data);
    text.textContent = copy;
    bubble.append(text);
    if (data.decisionReason) bubble.append(reasonText(data.decisionReason));
    if (openTask?.wait.reason === 'user_question') extras.push(questionActions(openTask));
    else if (openTask) {
      const open = node('div', 'event-actions question-actions');
      open.append(button('確認内容を開く', 'button', () => openDetail({ kind: 'wait', taskId: openTask.id, waitId: data.id })));
      extras.push(open);
    } else {
      const resolution = node('p', 'msg-resolution');
      resolution.append(icon('check'), document.createTextNode(closedLabels[context.closed.get(data.id)] ?? '確認待ちは終了しました'));
      extras.push(resolution);
    }
  } else if (event.kind === 'report') {
    copy = data.report || 'セッションから報告がありました';
    text.textContent = copy;
    const link = linkToRun(data.run);
    if (link) {
      link.className = 'run-link';
      link.title = 'T3 で開く';
      link.setAttribute('aria-label', 'T3 で開く');
      link.replaceChildren(icon('open'));
      text.append(link);
    }
    bubble.append(text);
    if (data.truncated) bubble.append(node('p', 'event-reason', '続きは T3 で確認できます。'));
  } else if (event.kind === 'completed') {
    bubble.classList.add('completed');
    text.append(icon('check', 'glyph kind-icon'), node('span', 'sr-only', '完了: '), document.createTextNode(data.text ?? ''));
    bubble.append(text);
    if (data.reason) bubble.append(reasonText(data.reason));
  } else {
    if (cron) text.append(icon('clock', 'glyph kind-icon'), node('span', 'sr-only', '定期依頼: '));
    text.append(document.createTextNode(data.text ?? ''));
    bubble.append(text);
    if (event.kind === 'user_answer') {
      const question = context.questions.get(data.waitId);
      if (question && !place.afterQuestion) {
        const quote = node('p', 'msg-quote');
        quote.append(icon('reply'), node('span', null, waitText(question.data)));
        quote.title = waitText(question.data);
        row.insertBefore(quote, meta.nextSibling);
      }
    }
    const blocker = event.kind === 'request' ? context.queued.get(event.taskId)?.blockedBy : null;
    if (blocker) {
      const owner = bots.find((item) => item.id === blocker.botId)?.name ?? blocker.botId;
      const note = node('p', 'msg-note', `#${blocker.id}（${owner}）の完了を待っています · ${blocker.request}`);
      note.title = blocker.request;
      extras.push(note);
    }
  }
  if (copy) {
    const control = iconButton('copy', 'テキストをコピー', () => void copyMessage(control, copy));
    actions.append(control);
  }
  if (openTask?.wait.reason === 'user_question') {
    const reply = iconButton('reply', '返信を書く', () => beginReply(openTask));
    reply.disabled = pendingWaits.has(openTask.wait.id);
    actions.append(reply);
  }
  line.append(bubble, actions);
  row.append(line, ...extras);
  return row;
}
function workingLine(data) {
  const row = node('div', 'msg bot working group-start');
  const line = node('div', 'msg-line');
  const dots = node('span', 'typing');
  dots.setAttribute('aria-hidden', 'true');
  dots.append(node('span'), node('span'), node('span'));
  let label = presenceLabels.thinking;
  if (data.presence === 'working') {
    const active = data.activeSessions?.[0];
    label = data.sessionTasks?.find((task) => task.id === active?.taskId)?.request ?? presenceLabels.working;
  }
  const text = node('span', 'working-label', label);
  text.title = label;
  line.append(avatar(data.bot, 'msg-avatar'), dots, text);
  row.append(line);
  return row;
}
function dayLabel(seconds) {
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const days = Math.round((today - new Date(seconds * 1000).setHours(0, 0, 0, 0)) / 86400000);
  return days === 0 ? '今日' : days === 1 ? '昨日' : dayFormat.format(seconds * 1000);
}

function timelineScrollPosition() {
  const scroller = elements.conversation;
  const height = scroller.scrollHeight;
  const top = scroller.scrollTop;
  return { height, top, atBottom: height - top - scroller.clientHeight < 65 };
}

function renderTimeline(scroll = 'auto', previousScroll = timelineScrollPosition()) {
  const older = elements.older;
  const data = view?.data;
  const events = [...(view?.events.values() ?? [])].sort((a, b) => a.id - b.id);
  older.hidden = !view?.olderBefore;
  older.disabled = !!view?.loadingOlder;
  older.textContent = view?.loadingOlder ? '読み込み中…' : '以前の会話を表示';
  if (!changed('timeline', view?.id, data?.bot, events, data?.waits, data?.queued, [...pendingWaits], listLoaded,
    data?.presence, data?.activeSessions, data?.sessionTasks, dayFormat.format(Date.now()))) return;
  const scroller = elements.conversation;
  const context = {
    bot: data?.bot ?? bots.find((item) => item.id === view?.id),
    waits: new Map((data?.waits ?? []).map((task) => [task.wait.id, task])),
    queued: new Map((data?.queued ?? []).map((task) => [task.id, task])),
    closed: new Map(events.filter((event) => event.kind === 'wait_closed').map((event) => [event.data.waitId, event.data.resolution])),
    questions: new Map(events.filter((event) => event.kind === 'waiting').map((event) => [event.data.id, event])),
  };
  const shown = events.filter((event) => event.kind !== 'wait_closed');
  const fragment = document.createDocumentFragment();
  let day = '';
  let previous = null;
  shown.forEach((event, index) => {
    const eventDay = dayLabel(event.time);
    if (eventDay !== day) {
      fragment.append(node('p', 'day', eventDay));
      day = eventDay;
      previous = null;
    }
    const side = messageSide(event);
    const next = shown[index + 1];
    const place = {
      groupStart: previous?.side !== side,
      groupEnd: !next || messageSide(next) !== side || dayLabel(next.time) !== day,
      taskStart: previous?.taskId !== event.taskId,
      afterQuestion: previous?.kind === 'waiting' && previous.waitId === event.data.waitId,
    };
    fragment.append(eventCard(event, context, place));
    previous = { side, taskId: event.taskId, kind: event.kind, waitId: event.data.id };
  });
  if (!fragment.childElementCount) {
    const empty = node('div', 'empty-state');
    if (data?.bot) empty.append(avatar(data.bot));
    const loaded = !!data || (!view && listLoaded);
    empty.append(node('h2', null, loaded ? (data ? data.bot.name : 'Bot がありません') : '読み込み中…'));
    if (data) empty.append(node('p', null, '新しい依頼を送ると、ここに会話が続きます。'));
    fragment.append(empty);
  }
  if (data && (data.presence === 'working' || data.presence === 'thinking')) fragment.append(workingLine(data));
  elements.timeline.replaceChildren(fragment);
  if (scroll === 'older') scroller.scrollTop = previousScroll.top + scroller.scrollHeight - previousScroll.height;
  else if (scroll === 'bottom' || previousScroll.atBottom) scroller.scrollTop = scroller.scrollHeight;
  else scroller.scrollTop = previousScroll.top;
}

function setNotice(context, text, isError = false) {
  notices.set(context.id, { text, isError });
  if (view?.id === context.id) renderComposer();
}
function renderComposer() {
  const context = view;
  elements['composer-container'].hidden = !context?.data;
  if (!context?.data) return;
  const reply = replies.get(context.id);
  const { text, isError } = notices.get(context.id) ?? {};
  const target = reply ? '質問への返信' : '新しい依頼';
  const body = reply ? reply.text : drafts.get(context.id) ?? '';
  const busy = pendingRequests.has(context.id) || (!!reply && pendingWaits.has(reply.waitId));
  elements['reply-context'].hidden = !reply;
  elements['reply-question'].textContent = reply ? `#${reply.taskId} ${reply.question}` : '';
  elements['compose-label'].textContent = target;
  elements['composer-mode'].textContent = target;
  elements['cancel-reply'].disabled = busy;
  elements.message.placeholder = reply ? '質問への回答を入力' : `${context.data.bot.name} に依頼する`;
  if (elements.message.value !== body) elements.message.value = body;
  elements.message.readOnly = busy;
  elements.send.disabled = busy || !body.trim() || byteLength(body) > (reply ? 8000 : 16000);
  elements.send.setAttribute('aria-label', reply ? '質問に返信する' : '新しい依頼を送る');
  elements.notice.textContent = text ?? '';
  elements.notice.classList.toggle('error', !!isError);
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
  const previousScroll = timelineScrollPosition();
  renderComposer();
  renderTimeline(scroll, previousScroll);
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
  notices.delete(view.id);
  const limit = reply ? 8000 : 16000;
  if (byteLength(elements.message.value) > limit) setNotice(view, `入力は ${limit.toLocaleString('ja-JP')} バイトまでです。`, true);
  else renderComposer();
});
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
