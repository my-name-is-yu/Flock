import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { promisify } from 'node:util';

function fixtures() {
  const nativeMatchMedia = window.matchMedia.bind(window);
  window.noHoverFixture = Object.assign(new EventTarget(), { matches: true });
  window.reducedMotionFixture = Object.assign(new EventTarget(), { matches: false });
  window.matchMedia = (query) => {
    if (query === '(hover: none)') return window.noHoverFixture;
    if (query === '(prefers-reduced-motion: reduce)') return window.reducedMotionFixture;
    return nativeMatchMedia(query);
  };
  window.EventSource = undefined;
  const bot = { id: 'release', name: 'Release' };
  const peer = { id: 'triage', name: 'Triage' };
  const time = new Date(2026, 10, 20, 13).getTime() / 1000;
  const sessionCreatedAt = new Date(2026, 10, 20, 17, 30).toISOString();
  const RealDate = Date;
  const midnight = new RealDate(2026, 10, 21).getTime();
  let now = new RealDate(2026, 10, 20, 23).getTime();
  const midnightDelay = midnight - now;
  window.Date = class extends RealDate {
    constructor(...args) { super(...(args.length ? args : [now])); }
    static now() { return now; }
  };
  let midnightRefresh;
  const schedule = window.setTimeout.bind(window);
  window.setTimeout = (callback, delay, ...args) => {
    if (delay === midnightDelay) {
      midnightRefresh = callback;
      return 0;
    }
    return schedule(callback, delay, ...args);
  };
  window.rollSessionDate = () => {
    now = midnight;
    midnightRefresh?.();
  };
  window.returnAfterMidnight = () => {
    now = new RealDate(2026, 10, 22).getTime();
    document.dispatchEvent(new Event('visibilitychange'));
  };
  const sessionTasks = [
    { id: 17, generation: 1, request: 'Completed work', createdAt: time,
      sessions: [{ environmentId: 'local', threadId: 'thread', state: 'completed', createdAt: sessionCreatedAt }] },
    { id: 16, generation: 1, request: 'Historical work', createdAt: time,
      sessions: [{ environmentId: 'local', threadId: 'older', state: 'completed' }] },
  ];
  const events = [
    { id: 1, kind: 'conversation_message', time, data: { text: 'First message' } },
    { id: 2, kind: 'conversation_reply', time: time + 60, data: { text: 'Second message' } },
    { id: 3, kind: 'report', time: time + 1000, taskId: 17, data: {
      report: 'Report', status: 'completed', truncated: false, revisionId: 1, resultRunId: 'final',
      run: { link: `${location.origin}/run`, environmentId: 'local', threadId: 'thread', runId: 'launch' },
    } },
  ];
  window.completeReport = () => {
    events.push({ id: 4, kind: 'completed', time: time + 1001, taskId: 17, data: {
      text: 'Report', revisionId: 1, run: { ...events[2].data.run, runId: 'final' },
    } });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  const dm = (id, seconds, fromBotId, data) => ({ id, time: time + seconds, kind: 'bot_message', data: { fromBotId, ...data } });
  const older = dm(8, 0, 'release', { text: 'Earlier release message' });
  const peerEvents = [
    dm(9, 60, 'release', { text: 'Release starts the exchange' }),
    dm(10, 90, 'release', { forwardedMessage: { text: 'Forwarded source only', time: time - 60 } }),
    dm(11, 100, 'triage', { text: 'Triage replies' }),
    dm(12, 1000, 'triage', { text: 'Triage resumes after a break' }),
    dm(13, 12 * 3600, 'triage', { text: 'Triage follows up tomorrow' }),
  ];
  let activeSessions = [];
  window.showWorking = (request) => {
    activeSessions = request ? [{ taskId: 18, threadId: 'working', environmentId: 'local', state: 'running', request }] : [];
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.showWorkingTargets = () => {
    activeSessions = ['Local', 'Mini'].map((name) => ({ taskId: 19, threadId: `thread-${name.toLowerCase()}`,
      environmentId: `env-${name.toLowerCase()}`, name, state: 'running', request: 'Compare release checks' }));
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.showSuggestion = () => {
    activeSessions = [];
    events.push({ id: 5, kind: 'conversation_reply', time: time + 1002, data: {
      text: 'I can check the logs.', suggestions: ['Check the logs.'],
    } });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.fetch = async (path) => {
    if (path === '/api/bots') return Response.json([bot, peer]);
    if (path.startsWith('/api/peer-conversation?')) return Response.json(path.includes('before=')
      ? { messages: [older], hasOlder: false } : { messages: peerEvents, hasOlder: true });
    return Response.json({ bot, events, olderBefore: 0, presence: activeSessions.length ? 'working' : 'idle',
      activeSessions, sessionTasks, sessionsBefore: 0, sessionCursor: 0, waits: [],
      canDecide: true, composerPerson: null });
  };
}

async function exercise() {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const sessionRows = () => [...document.querySelectorAll('.session-row')];
  const sessionLabel = (row) => row?.querySelector('.activity-status span:last-child')?.textContent;
  const [finished, historical] = sessionRows();
  check(sessionLabel(finished) === '17:30' && finished.title.includes('17:30') &&
    sessionLabel(historical) === 'Complete' && !historical.title.includes('Started'),
    'The sidebar used the request time instead of the Session creation time');
  window.rollSessionDate();
  check(sessionLabel(sessionRows()[0]) === 'Yesterday', 'An open tab did not refresh Session time at local midnight');
  window.returnAfterMidnight();
  check(sessionLabel(sessionRows()[0]) === 'Nov 20', 'Returning to the tab did not refresh the relative Session time');
  window.showWorkingTargets();
  const destinationRows = () => sessionRows().filter((row) => ['Local', 'Mini'].some((name) =>
    row.querySelector('.activity-title')?.textContent.startsWith(`${name} · Compare release checks`)));
  for (let attempt = 0; attempt < 20 && destinationRows().length !== 2; attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  const destinations = destinationRows();
  check(destinations.length === 2 && destinations.every((row) => sessionLabel(row) === 'Working'),
    'Running destination Sessions did not show distinct visible names and live status');
  const sampleConversation = document.createElement('section');
  sampleConversation.className = 'conversation';
  sampleConversation.style.setProperty('--user-bubble', 'rgb(0, 122, 53)');
  const sampleMessage = document.createElement('article');
  sampleMessage.className = 'msg user event-message';
  const sampleBubble = document.createElement('div');
  sampleBubble.className = 'bubble';
  const sampleStatus = document.createElement('p');
  sampleStatus.className = 'event-reason';
  sampleStatus.textContent = 'Local · Working';
  const sampleLink = document.createElement('a');
  sampleLink.className = 'run-link';
  sampleLink.innerHTML = '<span class="glyph">↗</span>';
  sampleBubble.append(sampleStatus, sampleLink);
  sampleMessage.append(sampleBubble);
  sampleConversation.append(sampleMessage);
  document.body.append(sampleConversation);
  const rgb = (value) => value.match(/[\d.]+/g).slice(0, 3).map(Number).map((channel) => {
    const normalized = channel / 255;
    return normalized <= 0.04045 ? normalized / 12.92 : ((normalized + 0.055) / 1.055) ** 2.4;
  });
  const luminance = (value) => {
    const [red, green, blue] = rgb(value);
    return .2126 * red + .7152 * green + .0722 * blue;
  };
  const contrast = (foreground, background) => {
    const values = [luminance(foreground), luminance(background)].sort((a, b) => b - a);
    return (values[0] + .05) / (values[1] + .05);
  };
  const background = getComputedStyle(sampleBubble).backgroundColor;
  for (const element of [sampleStatus, sampleLink]) {
    const style = getComputedStyle(element);
    check(style.opacity === '1' && getComputedStyle(sampleBubble).opacity === '1' &&
      contrast(style.color, background) >= 4.5,
      `User-bubble destination text/link contrast is ${contrast(style.color, background).toFixed(2)}:1`);
  }
  const hoverRule = [...document.styleSheets].flatMap((sheet) => [...sheet.cssRules])
    .find((rule) => rule.selectorText === '.msg.user .bubble .run-link:hover');
  check(hoverRule?.style.background === 'transparent',
    'Hovering a destination link must preserve the user-bubble background');
  sampleConversation.remove();
  const rows = [...document.querySelectorAll('.event-message')];
  const [first, second, report] = rows.map(row => row.querySelector('.event-text'));
  const singleTime = rows.map(row => row.querySelector('time'));
  const shown = () => [...document.querySelectorAll('.event-message > time')];
  const point = (target, type, x = 10, timeStamp = 100) => {
    const event = new PointerEvent(type, { bubbles: true, cancelable: true, pointerType: 'touch',
      pointerId: 1, isPrimary: true, clientX: x, clientY: 10 });
    Object.defineProperty(event, 'timeStamp', { value: timeStamp });
    return target.dispatchEvent(event);
  };
  const click = (target, detail = 1) => target.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, detail }));
  const tap = (target) => {
    check(point(target, 'pointerdown') && point(target, 'pointerup'), 'The app canceled the browser pointer event');
    return click(target);
  };

  check(rows.length === 3 && !shown().length && document.querySelectorAll('.chat-time-break time').length === 1,
    'Resting view lost the group marker or revealed a message timestamp');
  click(first, 0);
  check(!shown().length, 'A keyboard or programmatic click toggled a timestamp');
  check(tap(first), 'The app canceled the bubble click');
  check(shown()[0] === singleTime[0] && rows[0].querySelector('.msg-line').nextElementSibling === singleTime[0],
    'Tapping a bubble did not move its semantic timestamp below the message');
  tap(second);
  check(shown().length === 1 && shown()[0] === singleTime[1], 'A second bubble did not close the first timestamp');

  const link = report.querySelector('a');
  const button = document.createElement('button');
  button.innerHTML = '<span>Action</span>';
  first.append(button);
  let activated = 0;
  for (const control of [link, button]) {
    control.addEventListener('click', (event) => {
      if (!event.defaultPrevented) activated++;
      event.preventDefault();
    });
    tap(control.firstElementChild);
  }
  check(activated === 2 && shown().length === 1 && shown()[0] === singleTime[1],
    'An interactive descendant lost its click or changed the shown timestamp');
  tap(second);
  check(!shown().length, 'Tapping the open bubble did not close its timestamp');

  point(first, 'pointerdown');
  point(first, 'pointermove', 40);
  point(first, 'pointerup');
  click(first);
  check(!shown().length, 'A swipe that returned to its start revealed a timestamp');
  point(first, 'pointerdown');
  point(first, 'pointerup', 10, 700);
  click(first);
  check(!shown().length, 'A long press revealed a timestamp');
  point(first, 'pointerdown');
  point(first, 'pointercancel');
  point(first, 'pointerup');
  click(first);
  check(!shown().length, 'A canceled pointer sequence revealed a timestamp');
  window.getSelection().selectAllChildren(first);
  tap(first);
  check(!shown().length && !window.getSelection().isCollapsed, 'A selection was changed or revealed a timestamp');
  window.getSelection().removeAllRanges();

  tap(first);
  check(rows.every(row => row.querySelectorAll('time').length === 1 && !row.querySelector('time').closest('[aria-hidden="true"]')),
    'The timestamp was duplicated or removed from accessibility');
  tap(report);
  rows[2].querySelector('[data-timeline-action=copy]').focus();
  window.completeReport();
  for (let attempt = 0; attempt < 20 && !document.getElementById('event-4'); attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  const merged = document.getElementById('event-4');
  check(merged && document.getElementById('event-3')?.closest('article') === merged,
    'Collapsing the report and completion dropped the original event anchor');
  check(document.getElementById('event-1') === rows[0] && document.activeElement.closest('article') === merged,
    'The live collapse replaced an unchanged row or dropped focus from the combined row');
  check(shown().length === 1 && shown()[0].parentElement === merged && merged.querySelectorAll('time').length === 1,
    'The revealed timestamp did not follow the collapsed row');
  check(merged.querySelectorAll('[aria-label="Reply to this result"]').length === 1 && !shown()[0].title.includes('#17'),
    'The collapsed row lost its single reply or leaked an internal Task number');
  tap(merged.querySelector('.event-text'));
  check(!shown().length && window.scrollY === 0, 'Tapping the collapsed row did not close its timestamp in the conversation');
  tap(document.querySelector('#event-1 .event-text'));
  window.noHoverFixture.matches = false;
  window.noHoverFixture.dispatchEvent(new Event('change'));
  tap(document.querySelector('#event-1 .event-text'));
  check(!shown().length, 'The tap timestamp remained enabled with hover support');

  const workingLabel = () => document.querySelector('.working-label:not(.leaving)');
  const leavingLabels = () => document.querySelectorAll('.working-label.leaving');
  const showWorking = async (text) => {
    window.showWorking(text);
    for (let attempt = 0; attempt < 20 && workingLabel()?.textContent !== text; attempt++)
      await new Promise(resolve => setTimeout(resolve, 10));
    check(workingLabel()?.textContent === text, 'The working status did not update');
  };
  await showWorking('Check staging');
  const workingRow = workingLabel().closest('.msg.working');
  await showWorking('Check production');
  check(workingLabel().closest('.msg.working') === workingRow && leavingLabels()[0]?.getAttribute('aria-hidden') === 'true',
    'Working text lost its stable row or left the old status exposed');
  await showWorking('Verify the result');
  check(leavingLabels().length === 1, 'Fast working updates left multiple outgoing labels');
  leavingLabels()[0].dispatchEvent(new AnimationEvent('animationcancel', { animationName: 'working-label-out' }));
  check(!leavingLabels().length, 'Cancelling the exit animation left an old status in the row');
  window.reducedMotionFixture.matches = true;
  await showWorking('Finish the check');
  check(!leavingLabels().length && !workingLabel().classList.contains('entering'),
    'Reduced-motion working statuses still entered or exited with animation');

  const ghost = document.getElementById('composer-ghost');
  const input = document.getElementById('message');
  const key = (name, options = {}) => input.dispatchEvent(new KeyboardEvent('keydown', {
    key: name, code: name, bubbles: true, cancelable: true, ...options,
  }));
  window.showSuggestion();
  for (let attempt = 0; attempt < 20 && ghost.hidden; attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  check(!ghost.hidden && !input.value, 'A suggested reply did not appear in the empty composer');
  check(key('Enter', { altKey: true }) && key('Tab', { ctrlKey: true }) && !input.value,
    'A modified keyboard shortcut accepted the suggested reply');
  input.dispatchEvent(new CompositionEvent('compositionstart', { bubbles: true }));
  check(ghost.hidden && key('Enter') && !input.value, 'A suggestion remained active during input composition');
  input.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true }));
  check(!ghost.hidden && !key('Tab') && input.value === 'Check the logs.' && ghost.hidden,
    'Tab did not accept the exact displayed suggestion');
  input.value = 'Check tomorrow’s logs.';
  input.dispatchEvent(new InputEvent('input', { bubbles: true }));
  document.getElementById('cancel-reply').click();
  check(ghost.hidden && !input.value, 'The suggestion covered a differently worded saved reply draft');

  window.noHoverFixture.matches = true;
  window.noHoverFixture.dispatchEvent(new Event('change'));
  history.pushState(null, '', '?bot=release&peer=triage');
  window.dispatchEvent(new PopStateEvent('popstate'));
  for (let attempt = 0; attempt < 20 && !document.getElementById('event-13'); attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  const [release, forwarded, triage] = document.querySelectorAll('.peer-dm-message');
  const senders = () => [...document.querySelectorAll('.peer-dm-message > .msg-meta:not(.sr-only) .msg-name')]
    .map(name => name.textContent).join(',');
  const eventTime = row => row.querySelector('.msg-actions > time');
  check(senders() === 'Release,Triage,Triage,Triage' && forwarded.querySelector('.msg-avatar'),
    'The read-only DM did not group by sender, gap, and local day');
  check(document.querySelectorAll('.day:not(.chat-time-break)').length === 2 &&
    document.querySelectorAll('.chat-time-break').length === 1 && document.getElementById('composer-container').hidden,
    'The read-only DM lost a centered separator or exposed the composer');
  check(release.tabIndex === 0 && release.querySelectorAll('time').length === 1 && eventTime(release).dateTime,
    'The peer message did not expose a focusable semantic timestamp');

  const forwardedTime = eventTime(forwarded);
  const source = forwarded.querySelector('.peer-forwarded-message');
  tap(source.querySelector('.event-text'));
  // The quote names only its original sender; the message's own time is the one revealed.
  check(shown()[0] === forwardedTime && !source.querySelector('time') &&
    source.querySelector('.msg-quote')?.textContent === 'Original sender',
    'Tapping a forwarded-only message did not reveal its own timestamp beside the attributed quote');
  tap(triage.querySelector('.event-text'));
  check(shown().length === 1 && shown()[0].parentElement === triage && eventTime(forwarded) === forwardedTime,
    'The peer message timestamp did not move back when another one was opened');
  tap(triage.querySelector('.event-text'));
  check(!shown().length, 'Tapping the open peer message did not close its timestamp');

  document.getElementById('older').click();
  for (let attempt = 0; attempt < 20 && !document.getElementById('event-8'); attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  check(document.getElementById('event-9') === release && release.querySelector('.msg-meta').classList.contains('sr-only') &&
    senders() === 'Release,Triage,Triage,Triage', 'Prepending peer history replaced the loaded row or repeated its sender label');
}

const assets = new URL('../assets/', import.meta.url);
const read = (path) => readFile(new URL(`.${path}`, assets));
const index = (await read('/index.html')).toString().replace('<script type="module" src="/app.js"></script>', `
  <script>(${fixtures})();</script>
  <script type="module">
    import '/app.js';
    try {
      for (let attempt = 0; attempt < 20 && document.querySelectorAll('.event-message').length < 3; attempt++)
        await new Promise(resolve => setTimeout(resolve, 10));
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script>`);
const contentTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
const server = createServer(async (request, response) => {
  const { pathname } = new URL(request.url, 'http://localhost');
  try {
    response.setHeader('Content-Type', pathname === '/' ? 'text/html' : contentTypes[extname(pathname)] ?? 'application/octet-stream');
    response.end(pathname === '/' ? index : await read(pathname));
  } catch {
    response.writeHead(404).end();
  }
});
const directory = await mkdtemp(join(tmpdir(), 'flock-message-time-dom-'));
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', `--user-data-dir=${directory}`, '--virtual-time-budget=1500', '--dump-dom',
    `http://127.0.0.1:${server.address().port}/?bot=release`], { encoding: 'utf8', timeout: 25000 });
  const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the timestamp checks');
} finally {
  server.close();
  await rm(directory, { recursive: true, force: true });
}
