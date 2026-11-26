import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function fixtures() {
  const nativeMatchMedia = window.matchMedia.bind(window);
  window.noHoverFixture = Object.assign(new EventTarget(), { matches: true });
  window.reducedMotionFixture = Object.assign(new EventTarget(), { matches: false });
  window.matchMedia = (query) => {
    if (query === '(hover: none)') return window.noHoverFixture;
    if (query === '(prefers-reduced-motion: reduce)') return window.reducedMotionFixture;
    return nativeMatchMedia(query);
  };
  let stream;
  window.EventSource = class extends EventTarget {
    constructor(path) {
      super();
      if (path === '/api/events?bot=release') stream = this;
    }
    close() {}
  };
  const bot = { id: 'release', name: 'Release', model: 'demo', peers: [{ id: 'triage' }] };
  const peer = { id: 'triage', name: 'Triage' };
  window.renameBot = (name) => {
    bot.name = name;
    document.dispatchEvent(new Event('visibilitychange'));
  };
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
    { id: 17, generation: 1, request: 'Review the release notes and report any regressions. '.repeat(40).trim(), createdAt: time,
      state: 'completed',
      sessions: [{ environmentId: 'local', threadId: 'thread', state: 'completed', createdAt: sessionCreatedAt }] },
    { id: 16, generation: 1, request: 'Historical work', createdAt: time, state: 'completed',
      sessions: [{ environmentId: 'local', threadId: 'older', state: 'completed' }] },
    { id: 15, generation: 1, request: 'Cancelled work', createdAt: time, state: 'cancelled',
      sessions: [{ environmentId: 'local', threadId: 'cancelled', state: 'completed', createdAt: sessionCreatedAt }] },
  ];
  const events = [
    { id: 1, kind: 'conversation_message', time, data: { text: 'First message' } },
    { id: 2, kind: 'conversation_reply', time: time + 60, data: { text: 'Second message' } },
    { id: 3, kind: 'report', time: time + 1000, taskId: 17, data: {
      report: 'Report', status: 'completed', truncated: false, revisionId: 1, resultRunId: 'final',
      run: { link: `${location.origin}/run`, environmentId: 'local', threadId: 'thread', runId: 'launch' },
    } },
  ];
  let holdView;
  window.replyMotionFixture = {
    ready() { stream.dispatchEvent(new Event('ready')); },
    change() { stream.dispatchEvent(new MessageEvent('change', { data: '{"bot":"release"}' })); },
    disconnect() { stream.dispatchEvent(new Event('error')); },
    reply(id, text) { events.push({ id, kind: 'conversation_reply', time: time + 1000 + id, data: { text } }); },
    holdNextView() { return new Promise(resolve => { holdView = resolve; }); },
  };
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
  let activeTasks = [];
  let parallelTasks = [];
  window.showWorking = (request) => {
    activeTasks = [{ id: 18, request, state: 'running', session: { threadId: 'working', environmentId: 'local' } }];
    parallelTasks = [];
    document.dispatchEvent(new Event('visibilitychange'));
  };
  const release = { id: 14, request: 'Prepare release' };
  window.showWorkingTargets = () => {
    activeTasks = [{ id: 19, request: 'Compare release checks', state: 'running' }, { ...release, state: 'running' }];
    parallelTasks = [{ id: 19, runs: ['Local', 'Mini'].map((name) => ({ threadId: `thread-${name.toLowerCase()}`,
      environmentId: `env-${name.toLowerCase()}`, name, state: 'running' })) }];
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.waitForReview = () => {
    activeTasks = activeTasks.map(task => task.id === release.id
      ? { ...task, state: 'external_waiting', waitingNote: 'Waiting for review on #412' } : task);
    window.replyMotionFixture.change();
  };
  window.finishReview = () => {
    activeTasks = activeTasks.filter(task => task.id !== release.id);
    sessionTasks.push({ ...release, state: 'completed', generation: 2, sessions: [] });
    window.replyMotionFixture.change();
  };
  window.showSuggestion = () => {
    activeTasks = [];
    parallelTasks = [];
    events.push({ id: 5, kind: 'conversation_reply', time: time + 1002, data: {
      text: 'I can check the logs.', suggestions: ['Check the logs.'],
    } });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.fetch = async (path) => {
    if (path === '/api/bots') return Response.json([bot, peer]);
    if (path.startsWith('/api/peer-conversation?')) return Response.json(path.includes('before=')
      ? { messages: [older], hasOlder: false } : { messages: peerEvents, hasOlder: true });
    const response = Response.json({ bot, events, olderBefore: 0, presence: activeTasks.some(task => task.state === 'running') ? 'working' : 'idle',
      activeTasks, parallelTasks, sessionTasks, sessionsBefore: 15, sessionCursor: 0, waits: [],
      canDecide: true, composerPerson: null, limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 } });
    if (holdView && path.startsWith('/api/view?')) {
      const held = holdView;
      holdView = undefined;
      await new Promise(resolve => held(resolve));
    }
    return response;
  };
}

async function exerciseReplyMotion() {
  const conversation = document.getElementById('conversation');
  const fixture = window.replyMotionFixture;
  const nextTask = () => new Promise(resolve => setTimeout(resolve, 0));

  const replyBubble = (id) => document.querySelector(`#event-${id} .bubble`);
  async function showReply(id, text, deliver) {
    conversation.scrollTop = conversation.scrollHeight;
    const rendered = new Promise(resolve => {
      const observer = new MutationObserver(() => {
        const bubble = replyBubble(id);
        if (bubble) {
          observer.disconnect();
          resolve(bubble);
        }
      });
      observer.observe(conversation, { childList: true, subtree: true });
    });
    fixture.reply(id, text);
    await deliver();
    const bubble = await rendered;
    if (!bubble.textContent.includes(text)) throw new Error(`Missing reply text: ${text}`);
    return bubble;
  }

  const ready = fixture.holdNextView();
  fixture.ready();
  const finishReady = await ready;
  const live = await showReply(6, 'A live reply', async () => {
    fixture.change();
    await nextTask();
    fixture.change();
    finishReady();
  });
  if (live.getAnimations().length === 0) throw new Error('A foreground live reply did not animate');

  async function withRefreshInFlight(id, text, catchUp) {
    const held = fixture.holdNextView();
    fixture.change();
    const finish = await held;
    return showReply(id, text, async () => {
      fixture.change();
      await nextTask();
      catchUp();
      finish();
    });
  }
  const resumed = await withRefreshInFlight(7, 'A reply recovered on return', () => {
    document.dispatchEvent(new Event('visibilitychange'));
    fixture.change();
  });
  if (resumed.getAnimations().length) throw new Error('A reply animated across visibility generations');

  const caughtUp = await withRefreshInFlight(8, 'A reply recovered after disconnect', () => fixture.disconnect());
  if (caughtUp.getAnimations().length) throw new Error('A reply animated during a coalesced disconnect catch-up');
}

async function exercise() {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const sessionRows = () => [...document.querySelectorAll('.session-row')];
  const sessionLabel = (row) => row?.querySelector('.activity-status span:last-child')?.textContent;
  const [finished, historical, cancelled] = sessionRows();
  check(sessionLabel(finished) === '17:30' && finished.dataset.tooltip.includes('17:30') &&
    finished.querySelector(':scope > .sr-only')?.textContent.includes('Nov 20, 2026, 17:30') &&
    sessionLabel(historical) === 'Complete' && !historical.dataset.tooltip.includes('Started'),
    'The sidebar used the request time instead of the Session creation time');
  check(sessionLabel(cancelled) === 'Cancelled · 17:30' && cancelled.dataset.tooltip.includes('17:30'),
    'The Done row showed the Session result instead of the cancelled request');
  finished.dispatchEvent(new PointerEvent('pointerover', { bubbles: true, pointerType: 'mouse' }));
  await new Promise(resolve => setTimeout(resolve, 500));
  const sessionTip = document.querySelector('.tooltip:popover-open .tooltip-label');
  const exactStart = 'Started Nov 20, 2026, 17:30';
  const start = sessionTip?.textContent.indexOf(exactStart) ?? -1;
  check(start >= 0, 'The Session tooltip omitted the exact start time');
  const startRange = document.createRange();
  startRange.setStart(sessionTip.firstChild, start);
  startRange.setEnd(sessionTip.firstChild, start + exactStart.length);
  check(startRange.getBoundingClientRect().bottom <= sessionTip.getBoundingClientRect().bottom,
    'The long Session request clipped its exact start time');
  finished.dispatchEvent(new PointerEvent('pointerout', { bubbles: true, pointerType: 'mouse', relatedTarget: document.body }));

  window.rollSessionDate();
  check(sessionLabel(sessionRows()[0]) === 'Yesterday', 'An open tab did not refresh Session time at local midnight');
  window.returnAfterMidnight();
  check(sessionLabel(sessionRows()[0]) === 'Nov 20', 'Returning to the tab did not refresh the relative Session time');
  window.showWorkingTargets();
  const group = name => document.querySelector(`[data-session-group="${name}"]`);
  const destinationRows = () => [...document.querySelectorAll('.request-sessions .session-row')];
  for (let attempt = 0; attempt < 20 && destinationRows().length !== 2; attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  const destinations = destinationRows();
  check(group('working').querySelector(':scope > .request-task > .session-row .activity-title').textContent === 'Compare release checks' &&
    destinations.map(row => row.querySelector('.activity-title').textContent).join(',') === 'Local,Mini' &&
    destinations.every(row => sessionLabel(row) === 'Working'),
    'Running destination Sessions did not show distinct visible names and live status');
  check(group('working').textContent.includes('Prepare release'), 'The request was not displayed before waiting');
  window.replyMotionFixture.ready();
  window.waitForReview();
  for (let attempt = 0; attempt < 20 && !group('waiting'); attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  check([...document.querySelectorAll('[data-session-group]')].map(section => section.dataset.sessionGroup).join(',') === 'working,waiting,done' &&
    sessionLabel(group('waiting').querySelector('.session-row')) === 'Waiting for review on #412' &&
    !group('working').textContent.includes('Prepare release') && !group('done').textContent.includes('Prepare release'),
    'A waiting request must show the Bot’s note under Waiting alongside Working and Done');
  window.finishReview();
  for (let attempt = 0; attempt < 20 && group('waiting'); attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  check(!group('waiting') && group('done').textContent.includes('Prepare release'),
    'A Bot-completed request without a Session did not move to Done');
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
  check(merged.querySelectorAll('[aria-label="Reply to this result"]').length === 1 && !shown()[0].getAttribute('aria-description').includes('#17'),
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

  window.reducedMotionFixture.matches = false;
  await exerciseReplyMotion();

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
  const descriptionId = document.getElementById('peer-heading').getAttribute('aria-describedby');
  check(document.getElementById(descriptionId)?.textContent === 'Read-only conversation between Bots',
    'The Bot conversation heading lost its read-only description');
  const hideList = document.getElementById('toggle-chat-sidebar');
  const showList = document.getElementById('show-chat-sidebar');
  hideList.focus();
  hideList.click();
  document.getElementById('navigation').getAnimations().forEach(animation => animation.finish());
  const listButton = showList.getBoundingClientRect();
  check(document.activeElement === showList &&
    document.elementFromPoint(listButton.x + listButton.width / 2, listButton.y + listButton.height / 2)?.closest('button') === showList,
    'Show Bot list did not take focus or was covered by the Back button');
  showList.click();
  check(document.activeElement === hideList, 'Showing the Bot list dropped keyboard focus');

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

  document.getElementById('peer-parent-link').click();
  const picker = document.getElementById('peer-picker');
  for (let attempt = 0; attempt < 20 && picker.hidden; attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  check(!picker.hidden, 'The Bot conversation picker did not become available');
  picker.click();
  const title = document.getElementById('detail-title');
  check(title.textContent === 'Release talks with', 'The conversation picker did not name the current Bot');
  window.renameBot('Deploy');
  for (let attempt = 0; attempt < 20 && title.textContent !== 'Deploy talks with'; attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  check(title.textContent === 'Deploy talks with', 'The open conversation picker kept the old Bot name');
}

const index = (await readAsset('/index.html')).replace('<script type="module" src="/app.js"></script>', `
  <script>(${fixtures})();</script>
  <script type="module">
    import '/app.js';
    ${exerciseReplyMotion}
    try {
      for (let attempt = 0; attempt < 20 && document.querySelectorAll('.event-message').length < 3; attempt++)
        await new Promise(resolve => setTimeout(resolve, 10));
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script>`);
await withAssetServer({ '/': index }, async (origin) => {
  requirePass(await dumpDom(process.argv[2], `${origin}/?bot=release`, { budget: 2500 }), 'The browser did not finish the timestamp checks');
});
