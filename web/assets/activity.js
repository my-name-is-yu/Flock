import { actionLabel, approvalSummary } from './action-labels.mjs';
import { accentColor, presenceMotion, showAvatar } from './avatars.js';
import { pendingKindLabels, sessionEndLabels, stateLabels } from './conversation-copy.mjs';
import { node } from './dom.js';
import { botThreadUrl } from './bot-thread.js';
import {
  bots,
  byId,
  connection,
  conversationFind,
  dayFormat,
  decisions,
  elements,
  fullTime,
  listLoaded,
  openDetail,
  reducedMotion,
  sidebarIcons,
  view,
} from './app-state.js';
import { button, changed, openNavigation, runHref, waitLabel, waitText } from './app-core.js';
import { skeletonRows, triggerRowReveal } from './sidebar.js';
import { startedLabel } from './timeline.js';

let profileAvatar = null;
let profileKey = '';
let profileMotion;
let scopeKey = '';

export function renderHeader() {
  const bot = view?.data?.bot ?? bots.find((item) => item.id === view?.id);
  conversationFind.setContext(view?.data ? view : null);
  const profile = elements['profile-avatar-button'];
  const identity = elements['profile-identity'];
  identity.hidden = !bot;
  elements['profile-toolbar'].hidden = !bot;
  // The first-run empty state carries its own heading; a bare "Conversation" title would only repeat it.
  elements['profile-placeholder'].hidden = !!bot || (listLoaded && !bots.length);
  elements['bot-activity'].hidden = !bot;
  byId('conversation-bot-settings').hidden = !bot;
  if (bot) byId('conversation-bot-dms').href = botThreadUrl(bot.id);
  const nextScopeKey = bot ? JSON.stringify([bot.id, bot.name, bot.avatar]) : '';
  if (nextScopeKey !== scopeKey) {
    if (bot) elements['activity-scope-name'].textContent = `${bot.name} activity`;
    if (bot) document.documentElement.style.setProperty('--bot-accent', accentColor(bot));
    else document.documentElement.style.removeProperty('--bot-accent');
  }
  scopeKey = nextScopeKey;
  const pageTitle = bot ? `${bot.name} · Flock` : 'Flock';
  if (document.title !== pageTitle) document.title = pageTitle;
  if (!bot) {
    profileAvatar?.destroy();
    profileAvatar = null;
    profileKey = '';
    return;
  }
  const presence = bot.paused ? 'paused' : (view?.data?.presence ?? 'idle');
  const motion = presenceMotion(bot.paused ? 'idle' : presence);
  const look = { state: motion, followPointer: motion === 'idle' };
  const key = JSON.stringify([bot.id, bot.avatar, bot.paused]);
  if (key !== profileKey) {
    profileAvatar?.destroy();
    profileAvatar = showAvatar(elements['profile-avatar'], bot, { still: !!bot.paused, ...look });
    profileKey = key;
  } else if (profileMotion !== motion) {
    profileAvatar?.update(look);
  }
  profileMotion = motion;
  const offline = connection === 'offline' && !bot.paused;
  const status = offline ? 'Offline' : stateLabels[presence] ?? presence;
  const showStatus = offline || (presence !== 'idle' && !(presence === 'waiting' && decisions.count(bot.id)));
  const settingsLabel = `${bot.name} settings`;
  const name = elements['profile-name'];
  identity.dataset.state = offline ? 'offline' : presence;
  profile.setAttribute('aria-label', settingsLabel);
  profile.dataset.tooltip = settingsLabel;
  name.textContent = bot.name;
  elements['profile-status'].textContent = showStatus ? status : '';
  elements['profile-status'].hidden = !showStatus;
  if (showStatus) profile.setAttribute('aria-describedby', 'profile-status');
  else profile.removeAttribute('aria-describedby');
}

function activityRow(tag, className, title, status, statusClass) {
  const row = node(tag, `activity ${className}`);
  const heading = node('span', 'activity-title', title);
  const line = node('span', `activity-status ${statusClass}`.trim());
  line.append(node('span', 'activity-mark'), node('span', null, status));
  row.append(heading, line);
  return row;
}
function sessionRow(request, session, status, tone, startedAt) {
  const link = runHref(session);
  const title = [session.name, request].filter(Boolean).join(' · ');
  const row = activityRow(link ? 'a' : 'div', 'session-row', title, status, tone);
  const details = [Number.isFinite(startedAt) && `Started ${fullTime.format(startedAt)}`, session.commit].filter(Boolean);
  row.dataset.tooltip = [...details, title].filter(Boolean).join(' · ');
  if (details.length) row.append(node('span', 'sr-only', ` · ${details.join(' · ')}`));
  if (link) {
    row.href = link;
    row.target = '_blank';
    row.rel = 'noopener noreferrer';
    row.insertAdjacentHTML('beforeend', sidebarIcons.open);
    row.lastElementChild.classList.add('activity-open');
    row.append(node('span', 'sr-only', ' (Open in T3)'));
  }
  return row;
}
function conversationSummaries(compactions, open) {
  return compactions.map((compaction) => {
    const item = node('details', 'activity-details');
    item.dataset.inspector = `summary-${compaction.compactionId}`;
    item.open = open.has(item.dataset.inspector);
    const summary = node('summary', 'activity');
    const status = node('span', 'activity-status');
    status.append(node('span', 'activity-mark'), node('span', null, `Summary of ${compaction.originalCount} messages`));
    summary.append(node('span', 'activity-title', 'Earlier conversation'), status);
    item.append(summary, node('p', 'activity-details-body', compaction.summary));
    return item;
  });
}
function finishedSessionRow(request, session) {
  const startedAt = Date.parse(session.createdAt);
  const started = Number.isFinite(startedAt) ? startedLabel(startedAt) : '';
  const quiet = session.state === 'completed' && !!started;
  const end = Object.hasOwn(sessionEndLabels, session.state) ? sessionEndLabels[session.state] : sessionEndLabels.ended;
  const status = quiet ? started : [end, started].filter(Boolean).join(' · ');
  const tone = quiet ? 'quiet' : session.state === 'failed' ? 'failed' : '';
  const row = sessionRow(request, session, status, tone, startedAt);
  if (quiet) row.querySelector('.activity-mark').after(node('span', 'sr-only', 'Complete, started '));
  return row;
}
function requestRow(task, sessions, renderRow, renderSession = renderRow) {
  const taskStatus = { state: task.state };
  if (sessions.length < 2) return renderRow(task.request, sessions[0] ?? taskStatus);
  const item = node('div', 'request-task');
  const links = node('div', 'request-sessions');
  for (const session of sessions)
    links.append(renderSession('', { ...session, name: session.name ?? 'T3 Session' }));
  item.append(renderRow(task.request, taskStatus), links);
  return item;
}
function requestGroups(history, activeTasks, parallelTasks, canStop = false) {
  const runsByTask = new Map(parallelTasks.map(task => [task.id, task.runs.filter(run => run.threadId)]));
  const groups = { working: [], waiting: [], done: [] };
  const withStopAction = (item, task) => {
    if (!canStop || task.state === 'stopping') return item;
    const stop = button(null, 'icon-button request-stop', () => {
      openNavigation(false);
      openDetail({ kind: 'stop-task', taskId: task.id });
    });
    stop.innerHTML = sidebarIcons.stop;
    stop.setAttribute('aria-label', 'Stop request');
    stop.dataset.tooltip = 'Stop request';
    stop.dataset.tooltip = 'Stop request';
    if (item.classList.contains('request-task')) {
      item.classList.add('has-stop');
      item.append(stop);
      return item;
    }
    const entry = node('div', 'request-entry');
    entry.append(item, stop);
    return entry;
  };
  for (const task of [...activeTasks].sort((a, b) => b.id - a.id)) {
    const sessions = runsByTask.get(task.id) ?? (task.session ? [{ ...task.session, state: task.state }] : []);
    if (task.problem) {
      const renderProblem = (request, session) => {
        const label = waitLabel(task.problem.reason);
        if (runHref(session)) {
          const row = sessionRow(request, session, label, 'waiting');
          row.dataset.tooltip = [task.problem.text, row.dataset.tooltip].join(' · ');
          return row;
        }
        const title = [session.name, request].filter(Boolean).join(' · ');
        const row = activityRow('button', 'session-row', title, label, 'waiting');
        row.type = 'button';
        row.dataset.tooltip = [task.problem.text, title].filter(Boolean).join(' · ');
        row.addEventListener('click', () => {
          openNavigation(false);
          openDetail({ kind: 'wait', taskId: task.id, waitId: task.problem.id });
        });
        return row;
      };
      groups.waiting.push(withStopAction(requestRow(task, sessions, renderProblem, finishedSessionRow), task));
      continue;
    }
    if (task.state === 'external_waiting') {
      const renderWaiting = (request, session) => {
        const row = sessionRow(request, session, task.waitingNote, 'waiting');
        row.dataset.tooltip = [task.waitingNote, row.dataset.tooltip].join(' · ');
        return row;
      };
      groups.waiting.push(withStopAction(requestRow(task, sessions, renderWaiting, finishedSessionRow), task));
      continue;
    }
    groups.working.push(withStopAction(requestRow(task, sessions, (request, session) => sessionRow(request, session,
      stateLabels[session.state] ?? session.state, session.state === 'confirmation_pending' ? 'waiting' :
        !Object.hasOwn(sessionEndLabels, session.state) ? 'live' : '',
      task.createdAt ? task.createdAt * 1000 : undefined)), task));
  }
  const finished = [...history].sort((a, b) => b.id - a.id);
  for (const task of finished) {
    const renderDone = (request, session) => finishedSessionRow(request, { ...session, state: task.state });
    groups.done.push(requestRow(task, task.sessions, renderDone, finishedSessionRow));
  }
  const sections = [];
  for (const [key, label] of [['working', 'Working'], ['waiting', 'Waiting'], ['done', 'Done']]) {
    const tasks = groups[key];
    if (!tasks.length) continue;
    const section = node('section', 'request-group');
    section.dataset.sessionGroup = key;
    const heading = node('h3', 'activity-group');
    heading.append(node('span', null, label), node('span', 'activity-group-count', tasks.length));
    section.append(heading, ...tasks);
    sections.push(section);
  }
  return sections;
}
let activityBotId = null;
let activitySeen = false;
let activityReveal = null;
let previousWaitsLoading = false;
let previousSessionsLoading = false;
export function renderActivity() {
  const data = view?.data;
  const loading = !!view && !data;
  const pending = view?.id ? decisions.pendingItemsFor(view.id) : [];
  const inboxWaitIds = new Set(pending.filter((item) => item.waitId > 0).map((item) => item.waitId));
  const problemWaitIds = new Set((data?.activeTasks ?? []).filter((task) => task.problem?.id > 0).map((task) => task.problem.id));
  const extraWaits = (data?.waits ?? []).filter((task) => !inboxWaitIds.has(task.wait.id) && !problemWaitIds.has(task.wait.id));
  if (changed('waits', view?.id, loading, pending, extraWaits)) {
    const revealedWaits = previousWaitsLoading && !loading;
    previousWaitsLoading = loading;
    const waitCount = pending.length + extraWaits.length;
    elements['wait-count'].textContent = waitCount;
    elements['wait-count'].hidden = !waitCount;
    elements['wait-list'].setAttribute('aria-busy', loading);
    const rows = pending.map((item) => {
      const status = pendingKindLabels[item.itemKind] ?? 'Needs you';
      const title = item.itemKind === 'policy_approval' && item.action
        ? [actionLabel(item.action), item.displayTarget ?? item.target].filter(Boolean).join(' · ') : item.requestTitle || item.request;
      const row = activityRow('button', 'wait-row', title, status, 'waiting');
      row.type = 'button';
      row.setAttribute('aria-description', item.itemKind === 'policy_approval' ? approvalSummary(item.prompt, item) || title : item.prompt || title);
      row.addEventListener('click', () => {
        openNavigation(false);
        decisions.openItem(item, row);
      });
      return row;
    });
    for (const task of extraWaits) {
      const row = activityRow('button', 'wait-row', task.request, waitLabel(task.wait.reason), 'waiting');
      row.type = 'button';
      row.setAttribute('aria-description', waitText(task.wait) || task.request);
      row.addEventListener('click', () => {
        openNavigation(false);
        openDetail({ kind: 'wait', taskId: task.id, waitId: task.wait.id });
      });
      rows.push(row);
    }
    elements['wait-list'].replaceChildren(...rows);
    if (loading) elements['wait-list'].append(...skeletonRows(1));
    else if (revealedWaits && rows.length) triggerRowReveal(elements['wait-list']);
    elements['wait-list'].closest('section').hidden = !loading && !rows.length;
  }
  const history = [...(view?.loadedSessions.values() ?? [])];
  const activeTasks = data?.activeTasks ?? [];
  const parallelTasks = data?.parallelTasks ?? [];
  const canStop = !!view?.composer;
  const more = elements['more-sessions'];
  more.hidden = !view?.sessionsBefore;
  more.disabled = !!view?.loadingSessions;
  if (changed('sessions', view?.id, loading, canStop, history, activeTasks, parallelTasks,
    data?.compactions, dayFormat.format(Date.now()))) {
    const revealedSessions = previousSessionsLoading && !loading;
    previousSessionsLoading = loading;
    const rows = requestGroups(history, activeTasks, parallelTasks, canStop);
    const list = elements['session-list'];
    const open = new Set(Array.from(list.querySelectorAll('details[open]'), (item) => item.dataset.inspector));
    rows.push(...conversationSummaries(data?.compactions ?? [], open));
    list.setAttribute('aria-busy', loading);
    list.replaceChildren(...rows);
    if (loading) list.append(...skeletonRows(2));
    else if (revealedSessions && rows.length) triggerRowReveal(list);
    list.closest('section').hidden = !loading && !rows.length && more.hidden;
  }
  const nextActivityBotId = view ? view.id : null;
  if (nextActivityBotId !== activityBotId) {
    const shouldReveal = activitySeen && !!nextActivityBotId;
    activityBotId = nextActivityBotId;
    if (nextActivityBotId) activitySeen = true;
    activityReveal?.cancel();
    activityReveal = null;
    const panel = elements['bot-activity'];
    if (shouldReveal && !panel.hidden && !reducedMotion.matches) {
      activityReveal = panel.animate?.([
        { opacity: 0, transform: 'translateY(6px)' },
        { opacity: 1, transform: 'none' },
      ], { duration: 190, easing: 'cubic-bezier(0.23, 1, 0.32, 1)' }) ?? null;
    }
  }
}
