import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function indexFixtures() {
  window.EventSource = undefined;
  const bot = { id: 'release', name: 'Release', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  const time = Math.floor(Date.now() / 1000) - 60;
  const events = [{ id: 1, kind: 'conversation_message', time, data: { text: 'Hello match' } }];
  window.fetch = async (path) => {
    if (path === '/api/bots') return Response.json([bot]);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: true });
    if (path.startsWith('/api/view?')) {
      return Response.json({
        bot, events, olderBefore: 0, presence: 'idle', activeSessions: [], activeTasks: [], parallelTasks: [],
        sessionTasks: [], sessionsBefore: 0, sessionCursor: 0, waits: [], canDecide: true,
        composerPerson: 'local-owner', limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
      });
    }
    if (path.startsWith('/api/conversation?')) return Response.json({ events, olderBefore: 0 });
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exerciseIndex() {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 50; attempt++) {
      if (predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    check(predicate(), message);
  }
  const clickBackdrop = (dialog) => {
    dialog.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, clientX: 4, clientY: 4 }));
    dialog.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: 4, clientY: 4 }));
  };
  const dragOutToBackdrop = (dialog, insideTarget) => {
    const rect = dialog.getBoundingClientRect();
    check(rect.width > 0 && rect.height > 0, 'Open dialog had an empty bounding rect');
    insideTarget.dispatchEvent(new PointerEvent('pointerdown', {
      bubbles: true, clientX: rect.left + 12, clientY: rect.top + 12,
    }));
    dialog.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: 4, clientY: 4 }));
  };

  await waitFor(() => $('#event-1') && $('#new-bot-button') && $('#profile-avatar-button') && $('#find-conversation'),
    'The conversation shell did not render');

  $('#new-bot-button').click();
  await waitFor(() => $('#bot-editor')?.open, 'Bot editor dialog did not open');
  dragOutToBackdrop($('#bot-editor'), $('#bot-editor').firstElementChild);
  check($('#bot-editor').open, 'Dragging out of #bot-editor closed the dialog');
  clickBackdrop($('#bot-editor'));
  check(!$('#bot-editor').open, 'Clicking the backdrop did not close #bot-editor');

  $('#profile-avatar-button').click();
  await waitFor(() => $('#detail')?.open && $('#bot-pause-setting') && $('#delete-bot-settings'),
    'Detail dialog did not open');
  dragOutToBackdrop($('#detail'), $('#detail-body'));
  check($('#detail').open, 'Dragging out of #detail closed the dialog');

  $('#bot-pause-setting').click();
  await waitFor(() => $('#bot-pause')?.open, 'Pause dialog did not open');
  $('#bot-pause-confirm').disabled = true;
  clickBackdrop($('#bot-pause'));
  check($('#bot-pause').open, 'Clicking backdrop closed #bot-pause while busy');
  $('#bot-pause-confirm').disabled = false;
  dragOutToBackdrop($('#bot-pause'), $('#bot-pause .dialog-head'));
  check($('#bot-pause').open, 'Dragging out of #bot-pause closed the dialog');
  clickBackdrop($('#bot-pause'));
  check(!$('#bot-pause').open, 'Clicking the backdrop did not close #bot-pause');

  $('#delete-bot-settings').click();
  await waitFor(() => $('#bot-delete')?.open, 'Delete dialog did not open');
  $('#bot-delete-confirm').disabled = true;
  clickBackdrop($('#bot-delete'));
  check($('#bot-delete').open, 'Clicking backdrop closed #bot-delete while busy');
  $('#bot-delete-confirm').disabled = false;
  clickBackdrop($('#bot-delete'));
  check(!$('#bot-delete').open, 'Clicking the backdrop did not close #bot-delete');

  clickBackdrop($('#detail'));
  check(!$('#detail').open, 'Clicking the backdrop did not close #detail');

  $('#find-conversation').click();
  await waitFor(() => !$('#conversation-find').hidden, 'Conversation find bar did not open');
  $('#event-1 .bubble').click();
  check(!$('#conversation-find').hidden, 'Clicking a message bubble unexpectedly dismissed find');
  $('#conversation').dispatchEvent(new MouseEvent('click', { bubbles: true }));
  check($('#conversation-find').hidden, 'Clicking empty conversation space did not dismiss find');
}

function settingsFixtures() {
  window.fetch = async (path, options = {}) => {
    if (path === '/api/settings') {
      return Response.json({
        mode: 'full', rules: [], canManagePolicy: true,
        model: { demo: true, connected: true }, t3Setup: null, personOnlyActions: [], t3: null,
      });
    }
    if (path === '/api/settings/installation') {
      return Response.json({
        access: { mode: 'local', users: [] },
        connections: {},
        system: {
          active: { port: 52011, remoteAddress: null },
          saved: { port: null, remoteAddress: null },
          next: { port: null, remoteAddress: null },
          overrides: { port: false, remoteAddress: false },
          pending: { port: false, remoteAddress: false },
        },
      });
    }
    if (path === '/api/settings/installation/access' && options.method === 'POST') {
      return Response.json({ ok: true, credential: { person: 'local-owner', password: 'secret-once-password' } });
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exerciseSettings() {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 50; attempt++) {
      if (predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    check(predicate(), message);
  }
  const clickBackdrop = (dialog) => {
    dialog.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, clientX: 4, clientY: 4 }));
    dialog.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: 4, clientY: 4 }));
  };
  const dragOutToBackdrop = (dialog, insideTarget) => {
    const rect = dialog.getBoundingClientRect();
    check(rect.width > 0 && rect.height > 0, 'Settings dialog had an empty bounding rect');
    insideTarget.dispatchEvent(new PointerEvent('pointerdown', {
      bubbles: true, clientX: rect.left + 12, clientY: rect.top + 12,
    }));
    dialog.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: 4, clientY: 4 }));
  };

  await waitFor(() => $('#system button') && $('#access button'), 'Installation settings did not render');

  $('#system button').click();
  await waitFor(() => $('#installation-editor')?.open, 'Installation editor did not open for system settings');
  dragOutToBackdrop($('#installation-editor'), $('#installation-editor form'));
  check($('#installation-editor').open, 'Dragging out of #installation-editor closed the dialog');
  clickBackdrop($('#installation-editor'));
  check(!$('#installation-editor').open, 'Clicking the backdrop did not close #installation-editor');

  $('#access button').click();
  await waitFor(() => $('#installation-editor')?.open && $('#installation-editor-title')?.textContent === 'Administrator password',
    'One-time credential modal did not open');
  clickBackdrop($('#installation-editor'));
  check($('#installation-editor').open, 'Clicking backdrop dismissed the one-time credential modal');
  $('#installation-editor-close').click();
  check(!$('#installation-editor').open, 'Dismiss button did not close the credential modal');
}

const wrapPage = (html, scriptSrc, fixturesFn, exerciseFn) => html.replace(
  `<script type="module" src="${scriptSrc}"></script>`,
  `<script>(${fixturesFn})();</script>
  <script type="module">
    import '${scriptSrc}';
    try {
      await (${exerciseFn})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error?.stack || error?.message || String(error);
    }
  </script>`,
);
const index = wrapPage(await readAsset('/index.html'), '/app.js', indexFixtures, exerciseIndex);
const settings = wrapPage(await readAsset('/settings.html'), '/settings.js', settingsFixtures, exerciseSettings);
await withAssetServer({ '/': index, '/settings': settings }, async (origin) => {
  for (const path of ['/?bot=release', '/settings']) {
    requirePass(await dumpDom(process.argv[2], `${origin}${path}`), 'Browser did not finish backdrop dismiss checks', path);
  }
});
