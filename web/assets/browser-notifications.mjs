import { coalescedRefresh, createLiveUpdates } from './live-updates.js';

const preference = 'flock-notifications';
const summaries = {
  user_question: 'Has a question for you.',
  policy_approval: 'Needs your approval.',
  pr_review: 'Has a pull request for you to review.',
  reply: 'Replied in the conversation.',
  failure: 'Something went wrong.',
};

export function notificationPermission() {
  return typeof globalThis.Notification?.requestPermission === 'function' ? Notification.permission : 'unsupported';
}

export function browserNotificationsEnabled() {
  try { return notificationPermission() === 'granted' && localStorage.getItem(preference) === 'on'; }
  catch { return false; }
}

export async function setBrowserNotifications(enabled) {
  if (enabled && notificationPermission() === 'default') await Notification.requestPermission();
  if (!enabled || notificationPermission() === 'granted') {
    if (enabled) localStorage.setItem(preference, 'on');
    else localStorage.removeItem(preference);
    window.dispatchEvent(new Event(preference));
  }
}

export function onNotificationPreferenceChange(update) {
  window.addEventListener(preference, update);
  window.addEventListener('storage', ({ key }) => {
    if (key === preference || key === null) update();
  });
}

function eventKey({ botId, kind, id }) {
  return JSON.stringify([botId, kind, id]);
}

function viewLock(botId) {
  return `${preference}:view:${botId}`;
}

function showNotification({ botId, botName, kind }, tag, openBot) {
  const notice = new Notification(botName, { body: summaries[kind] ?? 'Needs your input.', tag, icon: '/flock.svg' });
  notice.onclick = (event) => {
    event.preventDefault();
    notice.close();
    window.focus();
    openBot(botId);
  };
}

function addJournalRange(ranges, after, through) {
  if (after >= through) return ranges;
  const merged = [];
  for (const [start, end] of [...ranges, [after, through]].sort((a, b) => a[0] - b[0])) {
    const previous = merged.at(-1);
    if (previous && start <= previous[1]) previous[1] = Math.max(previous[1], end);
    else merged.push([start, end]);
  }
  return merged;
}

async function deliverNotifications({ inbox, events, after, through, scope }, viewer, viewingBot, openBot) {
  if (!inbox.length && !events.length) return;
  const history = `${preference}:delivered:${viewer}`;
  const locks = navigator.locks;
  const deliver = async () => {
    let deliveredInbox = new Set();
    let deliveredJournal = new Map();
    try {
      const saved = JSON.parse(localStorage.getItem(history));
      deliveredInbox = new Set(saved?.inbox);
      deliveredJournal = new Map(saved?.journal);
    } catch {}
    const activeViews = await locks?.query();
    if (!browserNotificationsEnabled()) return;
    const selected = document.hidden ? null : viewingBot();
    const handle = (event, key) => {
      const viewing = selected === event.botId || activeViews?.held.some(({ name }) => name === viewLock(event.botId));
      if (!viewing) {
        try { showNotification(event, `${history}:${key}`, openBot); } catch { return false; }
      }
      return true;
    };
    for (const item of inbox) {
      const key = eventKey(item);
      if (!deliveredInbox.has(key) && handle(item, key)) deliveredInbox.add(key);
    }
    const previousByBot = new Map();
    for (const event of events) {
      const { botId, id } = event;
      const ranges = deliveredJournal.get(botId) ?? [];
      const previous = previousByBot.get(botId) ?? Math.max(after, scope[botId]);
      const handled = ranges.some(([start, end]) => start < id && id <= end);
      if (handled || handle(event, eventKey(event)))
        deliveredJournal.set(botId, addJournalRange(ranges, previous, id));
      previousByBot.set(botId, id);
    }
    for (const [botId, id] of previousByBot) {
      const ranges = deliveredJournal.get(botId) ?? [];
      deliveredJournal.set(botId, addJournalRange(ranges, id, through));
    }
    try {
      localStorage.setItem(history, JSON.stringify({ inbox: [...deliveredInbox], journal: [...deliveredJournal] }));
    } catch {}
  };
  if (locks) await locks.request(history, deliver);
  else await deliver();
}

export function createBrowserNotifications({
  viewingBot = () => null,
  openBot = bot => location.assign(`/?${new URLSearchParams({ bot })}`),
} = {}) {
  let state = null;
  let visibleBot = null;
  let releaseView;
  let retry;

  function updateViewing(bot = !document.hidden && browserNotificationsEnabled() ? viewingBot() : null) {
    if (bot === visibleBot) return;
    releaseView?.();
    releaseView = null;
    visibleBot = bot;
    if (!bot || !navigator.locks) return;
    const viewing = new Promise(resolve => { releaseView = resolve; });
    void navigator.locks.request(viewLock(bot), { mode: 'shared' }, () => viewing).catch(() => {});
  }

  const read = coalescedRefresh(async () => {
    const current = state;
    if (!current) return;
    try {
      let more;
      do {
        const body = current.after == null ? {} : { after: current.after, scope: current.scope };
        const response = await fetch('/api/notifications', {
          method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Flock-Action': '1' },
          body: JSON.stringify(body),
        });
        if (response.status === 401 || response.status === 403) {
          if (current === state) state = null;
          return;
        }
        if (!response.ok) throw new Error('Could not load notifications');
        const viewer = response.headers.get('X-Flock-Viewer');
        const page = await response.json();
        if (current !== state || !browserNotificationsEnabled()) return;
        if (current.after != null && viewer !== current.viewer) {
          current.after = null;
          more = true;
          continue;
        }
        const inbox = new Set(page.inbox.map(eventKey));
        if (current.after != null) {
          const added = page.inbox.filter(item => Object.hasOwn(current.scope, item.botId) &&
            !current.inbox.has(eventKey(item)));
          await deliverNotifications({ ...page, inbox: added, after: current.after }, viewer, viewingBot, openBot);
        }
        current.viewer = viewer;
        current.after = page.through;
        current.scope = page.scope;
        current.inbox = inbox;
        more = page.more;
      } while (more && current === state);
    } catch {
      if (current === state) {
        clearTimeout(retry);
        retry = setTimeout(refresh, 2500);
      }
    }
  });

  function refresh() {
    clearTimeout(retry);
    if (!browserNotificationsEnabled()) { state = null; return; }
    state ??= { after: null };
    return read();
  }

  window.addEventListener('pagehide', () => updateViewing(null));
  return { refresh, updateViewing };
}

export function listenForBrowserNotifications(viewingBot) {
  const notifications = createBrowserNotifications({ viewingBot });
  const live = createLiveUpdates(notifications.refresh, change => {
    if (change.list) void notifications.refresh();
  });
  const update = () => {
    notifications.updateViewing();
    live.use(browserNotificationsEnabled() ? '/api/events' : null);
    void notifications.refresh();
  };
  onNotificationPreferenceChange(update);
  document.addEventListener('visibilitychange', update);
  window.addEventListener('pagehide', () => live.use(null));
  window.addEventListener('pageshow', update);
  update();
}
