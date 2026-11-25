import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createBrowserNotifications, setBrowserNotifications } from '../assets/browser-notifications.mjs';

const release = { botId: 'release', botName: 'Release' };
const question = { ...release, id: 7, kind: 'user_question' };
const approval = { ...release, id: 7, kind: 'policy_approval' };

function browser(t, permission = 'granted') {
  const entries = new Map();
  const notices = [];
  const requests = [];
  const queued = new Map();
  const held = [];
  const window = new EventTarget();
  const document = { hidden: false };
  const navigation = [];
  let permissionRequests = 0;
  let page = { through: 10, scope: { release: 10 }, inbox: [question], events: [] };
  class Notification {
    static permission = permission;
    static async requestPermission() { permissionRequests++; return this.permission = 'granted'; }
    constructor(title, options) { this.title = title; this.options = options; notices.push(this); }
    close() { navigation.push('close'); }
  }
  window.focus = () => navigation.push('focus');
  const locks = {
    request(name, options, callback) {
      if (options.mode === 'shared') {
        const lock = { name };
        held.push(lock);
        return callback().finally(() => held.splice(held.indexOf(lock), 1));
      }
      const next = (queued.get(name) ?? Promise.resolve()).then(options);
      queued.set(name, next);
      return next;
    },
    async query() { return { held: [...held] }; },
  };

  const globals = {
    Notification, window, document, navigator: { locks }, location: { assign: url => navigation.push(url) },
    localStorage: { getItem: key => entries.get(key) ?? null,
      setItem: (key, value) => entries.set(key, value), removeItem: key => entries.delete(key) },
    fetch: async (url, request) => {
      assert.equal(request.method, 'POST');
      requests.push([url, JSON.parse(request.body)]);
      return Response.json(page, { headers: { 'X-Flock-Viewer': '"person"' } });
    },
  };
  for (const [name, value] of Object.entries(globals)) {
    const previous = Object.getOwnPropertyDescriptor(globalThis, name);
    Object.defineProperty(globalThis, name, { configurable: true, value });
    t.after(() => {
      if (previous) Object.defineProperty(globalThis, name, previous);
      else delete globalThis[name];
    });
  }
  return { notices, requests, document, navigation, permissionRequests: () => permissionRequests,
    page(inbox, events = [], through = page.through + 1, scope = page.scope) { page = { through, scope, inbox, events }; } };
}

test('loads a baseline only after explicit opt-in and ignores prior inbox items', async t => {
  const simulated = browser(t, 'default');
  const notifications = createBrowserNotifications();
  await notifications.refresh();
  assert.equal(simulated.permissionRequests(), 0);
  assert.deepEqual(simulated.requests, []);
  await setBrowserNotifications(true);
  await notifications.refresh();
  assert.equal(simulated.permissionRequests(), 1);
  assert.deepEqual(simulated.requests, [['/api/notifications', {}]]);
  assert.deepEqual(simulated.notices, []);
  simulated.page([question, approval]);
  await notifications.refresh();
  assert.deepEqual(simulated.requests.at(-1), ['/api/notifications', { after: 10, scope: { release: 10 } }]);
  assert.deepEqual(simulated.notices.map(notice => notice.options.body), ['Needs your approval.']);

  let prevented = false;
  simulated.notices[0].onclick({ preventDefault() { prevented = true; } });
  assert.equal(prevented, true);
  assert.deepEqual(simulated.navigation, ['close', 'focus', '/?bot=release']);

  await setBrowserNotifications(false);
  await notifications.refresh();
  assert.equal(simulated.requests.length, 2);
});

test('starts alerts for a newly visible Bot after accepting its inbox and journal baseline', async t => {
  const simulated = browser(t);
  const notifications = createBrowserNotifications();
  const triage = { botId: 'triage', botName: 'Triage' };
  const sharedQuestion = { ...triage, id: 25, kind: 'user_question' };
  simulated.page([], [], 0, {});
  await setBrowserNotifications(true);
  await notifications.refresh();

  simulated.page([sharedQuestion], [], 0, { triage: 24 });
  await notifications.refresh();
  assert.deepEqual(simulated.requests.at(-1), ['/api/notifications', { after: 0, scope: {} }]);
  assert.deepEqual(simulated.notices, []);

  simulated.page([sharedQuestion, { ...triage, id: 26, kind: 'policy_approval' }],
    [{ ...triage, id: 27, kind: 'reply' }], 27);
  await notifications.refresh();
  assert.deepEqual(simulated.requests.at(-1), ['/api/notifications', { after: 0, scope: { triage: 24 } }]);
  assert.deepEqual(simulated.notices.map(notice => notice.options.body), ['Needs your approval.', 'Replied in the conversation.']);
});

test('alerts only when hidden or viewing a different Bot', async t => {
  const simulated = browser(t);
  let selected = 'release';
  const notifications = createBrowserNotifications({
    viewingBot: () => selected,
    openBot: bot => simulated.navigation.push({ bot }),
  });
  await setBrowserNotifications(true);
  await notifications.refresh();

  simulated.page([question], [{ ...release, id: 11, kind: 'reply' }]);
  await notifications.refresh();
  assert.equal(simulated.notices.length, 0);

  simulated.document.hidden = true;
  simulated.page([question], [{ ...release, id: 12, kind: 'reply' }]);
  await notifications.refresh();
  assert.equal(simulated.notices.length, 1);

  simulated.document.hidden = false;
  selected = 'planning';
  simulated.page([question], [{ ...release, id: 13, kind: 'failure' }]);
  await notifications.refresh();
  assert.deepEqual(simulated.notices.map(notice => [notice.title, notice.options.body]), [
    ['Release', 'Replied in the conversation.'], ['Release', 'Something went wrong.'],
  ]);
  simulated.notices[1].onclick({ preventDefault() {} });
  assert.deepEqual(simulated.navigation, ['close', 'focus', { bot: 'release' }]);
});

test('delivers each event once across tabs and suppresses alerts for a Bot visible in another tab', async t => {
  const simulated = browser(t);
  const first = createBrowserNotifications({ viewingBot: () => 'planning' });
  const second = createBrowserNotifications();
  await setBrowserNotifications(true);
  await Promise.all([first.refresh(), second.refresh()]);

  const reply = { ...release, id: 11, kind: 'reply' };
  simulated.page([question, approval], [reply]);
  await Promise.all([first.refresh(), second.refresh()]);
  assert.equal(simulated.notices.length, 2);
  assert.equal(new Set(simulated.notices.map(notice => notice.options.tag)).size, 2);

  simulated.page([question, approval], [reply]);
  await second.refresh();
  assert.equal(simulated.notices.length, 2);

  first.updateViewing('release');
  simulated.page([question, approval], [{ ...release, id: 13, kind: 'reply' }]);
  await second.refresh();
  assert.equal(simulated.notices.length, 2);

  first.updateViewing(null);
  simulated.page([question, approval], [{ ...release, id: 14, kind: 'reply' }]);
  await second.refresh();
  assert.equal(simulated.notices.length, 3);
});

test('a suspended tab can replay multiple pages without displaying alerts twice', async t => {
  const simulated = browser(t);
  const active = createBrowserNotifications();
  const suspended = createBrowserNotifications();
  await setBrowserNotifications(true);
  await Promise.all([active.refresh(), suspended.refresh()]);
  for (const tab of [active, suspended]) {
    for (const after of [10, 110, 210]) {
      const events = Array.from({ length: 100 }, (_, index) => ({ ...release, id: after + index + 1, kind: 'reply' }));
      simulated.page([question], events, after + 100);
      await tab.refresh();
    }
  }
  assert.equal(simulated.notices.length, 300);
  assert.equal(new Set(simulated.notices.map(notice => notice.options.tag)).size, 300);
});

test('a newer tab baseline leaves earlier undelivered alerts available to older tabs', async t => {
  const simulated = browser(t);
  const older = createBrowserNotifications();
  const newer = createBrowserNotifications();
  await setBrowserNotifications(true);
  await older.refresh();
  simulated.page([question], [], 15);
  await newer.refresh();
  const latest = { ...release, id: 16, kind: 'reply' };
  simulated.page([question], [latest], 16);
  await newer.refresh();
  const earlier = { ...release, id: 15, kind: 'reply' };
  simulated.page([question], [earlier, latest], 16);
  await older.refresh();
  assert.equal(simulated.notices.length, 2);
  assert.notEqual(simulated.notices[0].options.tag, simulated.notices[1].options.tag);
});
