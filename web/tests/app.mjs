import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { setImmediate } from 'node:timers/promises';
import { test } from 'node:test';
import { runInNewContext } from 'node:vm';

const app = readFileSync(new URL('../assets/app.js', import.meta.url), 'utf8').replace(/^import .*;\n/gm, '');

class Element extends EventTarget {
  constructor(fragment = false) {
    super();
    this.fragment = fragment;
  }
  children = [];
  attributes = new Map();
  dataset = {};
  classList = { add() {}, toggle() {}, contains() { return false; } };
  style = { setProperty() {}, removeProperty() {} };
  get childElementCount() { return this.children.length; }
  append(...children) {
    for (const child of children) {
      if (child?.fragment) this.children.push(...child.children);
      else this.children.push(child);
    }
  }
  replaceChildren(...children) {
    this.children = [];
    this.append(...children);
  }
  setAttribute(key, value) { this.attributes.set(key, String(value)); }
  focus() {}
  contains() { return false; }
  querySelectorAll() { return []; }
  click() { this.dispatchEvent(new Event('click')); }
  *descendants() {
    for (const child of this.children) {
      if (!(child instanceof Element)) continue;
      yield child;
      yield* child.descendants();
    }
  }
}

function storage() {
  const values = new Map();
  return {
    getItem: (key) => values.get(key) ?? null,
    setItem: (key, value) => values.set(key, value),
    removeItem: (key) => values.delete(key),
  };
}

function loadPage({ location = new URL('http://localhost/?bot=demo'), sessionStorage = storage(), fetch }) {
  const elements = new Map();
  const get = (id) => {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
  };
  const document = Object.assign(new EventTarget(), {
    getElementById: get,
    querySelector: get,
    createElement: () => new Element(),
    createDocumentFragment: () => new Element(true),
    createTextNode: (text) => text,
  });
  let poll;
  runInNewContext(app, {
    document, location, sessionStorage, fetch, URL, URLSearchParams, TextEncoder, crypto,
    history: {
      pushState(_state, _title, url) { location.href = url.href; },
      replaceState(_state, _title, url) { location.href = url.href; },
    },
    localStorage: storage(),
    window: new EventTarget(),
    navigator: { platform: 'MacIntel' },
    matchMedia: () => new EventTarget(),
    ResizeObserver: class { observe() {} },
    setInterval(callback, milliseconds) {
      assert.equal(milliseconds, 2500);
      poll = callback;
    },
    setTimeout() { return 1; },
    accentColor() {},
    showAvatar() {},
    createBotEditor() {},
  }, { filename: 'app.js' });
  return { get, poll };
}

const demo = { id: 'demo', name: 'Demo', peers: [{ id: 'peer' }] };
const peer = { id: 'peer', name: 'Peer' };
const ok = (body) => ({ ok: true, json: async () => body });
const page = (events, olderBefore = 0) => ({ events, olderBefore });
const view = (history) => ({ ...history, bot: demo, waits: [], presence: 'idle' });
const event = (id, kind, data) => ({ id, kind, data, taskId: 1, time: 1700000000 });

test('editing and sending a revised reply preserves the original pending text and receipt after reload', async () => {
  const sessionStorage = storage();
  const submissions = [];
  const stopped = event(1, 'cancelled', { request: 'Run a demo task' });
  const fetch = async (path, init) => {
    if (path === '/api/bots') return ok([demo]);
    if (path === '/api/view?bot=demo') return ok(view(page([stopped])));
    assert.equal(path, '/api/messages');
    submissions.push(JSON.parse(init.body));
    if (submissions.length === 1) throw new Error('Lost acknowledgement');
    return ok({});
  };
  const { get } = loadPage({ sessionStorage, fetch });
  await setImmediate();
  const reply = [...get('timeline').descendants()].find((node) =>
    node.attributes.get('aria-label') === 'Reply to this stopped request');
  assert.ok(reply);
  reply.click();
  for (const text of ['Explain this result', 'When did this stop?']) {
    get('message').value = text;
    get('message').dispatchEvent(new Event('input'));
    get('composer').dispatchEvent(new Event('submit', { cancelable: true }));
    await setImmediate();
  }
  assert.equal(submissions.length, 2);
  assert.notEqual(submissions[0].clientMessageId, submissions[1].clientMessageId);

  const reload = loadPage({ sessionStorage, fetch });
  await setImmediate();
  assert.equal(reload.get('message').value, 'Explain this result');
  assert.equal(reload.get('send').attributes.get('aria-label'), 'Retry submission');
  reload.get('composer').dispatchEvent(new Event('submit', { cancelable: true }));
  await setImmediate();
  assert.equal(submissions.length, 3);
  assert.deepEqual(submissions[2], submissions[0]);
  assert.equal(submissions[2].replyToMessageId, stopped.id);
});

for (const dm of [false, true]) {
  test(`${dm ? 'Bot DM' : 'chat'} catch-up completes across polling ticks`, async () => {
    const location = new URL(`http://localhost/?bot=demo${dm ? '&peer=peer' : ''}`);
    const latestPath = dm ? '/api/peer-conversation?bot=demo&peer=peer' : '/api/view?bot=demo';
    const historyPath = dm ? latestPath : '/api/conversation?bot=demo';
    const row = (id) => event(id, dm ? 'bot_message' : 'conversation_reply',
      { text: `Message ${id}`, fromBotId: 'peer', toBotId: 'demo' });
    const older = new Map([3, 2].map((cursor) => [cursor, Promise.withResolvers()]));
    const requests = [];
    let missedPages = false;
    const fetch = async (path) => {
      if (path === '/api/bots') return ok([demo, peer]);
      if (dm && path === '/api/view?bot=demo') return ok(view(page([])));
      requests.push(path);
      if (path === latestPath) {
        const latest = missedPages ? page([row(3)], 3) : page([row(1)]);
        return ok(dm ? latest : view(latest));
      }
      const cursor = Number(new URL(path, location).searchParams.get('before'));
      return ok(await older.get(cursor).promise);
    };
    const { get, poll } = loadPage({ location, fetch });
    await setImmediate();
    missedPages = true;
    poll();
    await setImmediate();
    poll();
    await setImmediate();
    older.get(3).resolve(page([row(2)], 2));
    await setImmediate();
    poll();
    await setImmediate();
    older.get(2).resolve(page([row(1)]));
    await setImmediate();
    assert.deepEqual(requests, [latestPath, latestPath, `${historyPath}&before=3`, `${historyPath}&before=2`]);
    const messages = [...get('timeline').descendants()].map((node) => node.textContent)
      .filter((text) => text?.startsWith('Message '));
    assert.deepEqual(messages, ['Message 1', 'Message 2', 'Message 3']);

    poll();
    await setImmediate();
    assert.equal(requests.at(-1), latestPath);
  });
}

test('switching Bots supersedes an in-flight catch-up', async () => {
  const older = Promise.withResolvers();
  let missedPages = false;
  const fetch = async (path) => {
    if (path === '/api/bots') return ok([demo, peer]);
    if (path === '/api/view?bot=demo') {
      const first = event(1, 'conversation_reply', { text: 'First reply' });
      const latest = event(3, 'conversation_reply', { text: 'Late reply' });
      return ok(view(missedPages ? page([latest], 3) : page([first])));
    }
    if (path === '/api/view?bot=peer') return ok({ ...view(page([])), bot: peer });
    assert.equal(path, '/api/conversation?bot=demo&before=3');
    return ok(await older.promise);
  };
  const { get, poll } = loadPage({ fetch });
  await setImmediate();
  missedPages = true;
  poll();
  await setImmediate();
  const otherBot = [...get('bot-list').descendants()].find((node) => node.dataset.focus === 'bot:peer');
  assert.ok(otherBot);
  otherBot.click();
  await setImmediate();
  assert.equal(get('profile-name').textContent, 'Peer');
  older.resolve(page([event(2, 'conversation_reply', { text: 'Earlier reply' })]));
  await setImmediate();
  assert.equal(get('profile-name').textContent, 'Peer');
  assert.ok([...get('timeline').descendants()].some((node) => node.textContent === 'Peer'));
});
