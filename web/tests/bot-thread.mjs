import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function fixtures() {
  window.EventSource = undefined;
  const scout = { id: 'scout', name: 'Scout', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  const reviewer = { id: 'reviewer', name: 'Reviewer', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  const time = Math.floor(Date.now() / 1000) - 600;
  const delivered = (id, fromBotId, fromName, text) => ({
    id, kind: 'conversation_message', taskId: 0, time: time + id, data: { text, botMessage: { fromBotId, fromName, depth: 1 } },
  });
  const events = [
    { id: 30, kind: 'conversation_message', taskId: 0, time: time + 30, data: { text: 'Ask Reviewer about the release' } },
    delivered(40, 'reviewer', 'Reviewer', 'Ready. See the report for every check that ran overnight and what is still pending.'),
    delivered(41, 'ghost', 'Ghost', 'A Bot this viewer cannot open'),
  ];
  const message = (id, fromBotId, textHtml) => ({ id, time: time + id, data: { fromBotId, textHtml } });
  window.dmRequests = [];
  window.fetch = async (path) => {
    if (path === '/api/bots') return Response.json([scout, reviewer]);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: true });
    if (path.startsWith('/api/view?')) return Response.json({
      bot: scout, events, olderBefore: 0, presence: 'idle', activeSessions: [], sessionTasks: [], sessionsBefore: 0,
      sessionCursor: 0, waits: [], canDecide: true, composerPerson: 'local-owner',
      limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
    });
    if (path.startsWith('/api/bot-dms?')) {
      const params = new URL(path, location.href).searchParams;
      dmRequests.push(params.toString());
      const pair = { bot: { id: 'scout', name: 'Scout' }, peer: { id: 'reviewer', name: 'Reviewer' } };
      if (!params.has('peer')) return Response.json({ bot: pair.bot, peers: [pair.peer] });
      if (params.has('before')) return Response.json({ ...pair, events: [message(12, 'scout', '<p>Check the release.</p>')], olderBefore: 0 });
      return Response.json({ ...pair, olderBefore: 39, hasNewer: params.has('anchor'), events: [
        message(39, 'scout', '<p>Is it ready?</p>'),
        message(40, 'reviewer', '<p><strong>Ready</strong>. See the <a href="https://example.com">report</a>.</p>'),
      ] });
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exercise() {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 100; attempt++) {
      if (predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    check(predicate(), message);
  }
  const panel = $('#bot-thread');
  const conversation = $('#conversation');
  const params = () => new URLSearchParams(location.search);
  if (params().has('thread')) {
    await waitFor(() => document.activeElement?.id === 'bot-thread-message-40', 'A thread address did not open its message');
    check(params().get('thread') === 'reviewer' && !panel.hidden, 'Loading a thread address dropped the thread');
    $('#bot-thread-close').click();
    await waitFor(() => panel.hidden && params().toString() === 'bot=scout', 'Closing a loaded thread kept it in the address');
    return;
  }

  await waitFor(() => $('#event-40') && $('#event-41'), 'Bot messages were missing from the conversation');
  const row = $('#event-40');
  check(row.matches('a.bot-message-row') && row.querySelector('.bot-message-name').textContent === 'Reviewer' &&
    row.querySelector('.avatar') && row.querySelector('.bot-message-preview').textContent.startsWith('Ready. See the report'),
    'A Bot message did not render as a compact sender row');
  check(row.getAttribute('href') === '/?bot=scout&thread=reviewer&message=40', 'The row did not link to its thread');
  check(getComputedStyle(row.querySelector('.bot-message-preview')).textOverflow === 'ellipsis', 'The preview was not truncated to one line');
  check($('#event-41').matches('article.bot-message-row') && !$('#event-41 a'), 'A message from an unavailable Bot became a link');

  row.click();
  await waitFor(() => $('.bot-thread-anchored'), 'Opening a Bot message did not show its thread');
  const anchored = $('.bot-thread-anchored');
  check(!panel.hidden && getComputedStyle(conversation).visibility === 'hidden', 'The thread did not replace the conversation in place');
  check(params().get('thread') === 'reviewer' && params().get('message') === '40', 'The thread was not in the address');
  check(anchored.id === 'bot-thread-message-40' && document.activeElement === anchored, 'The opened message was not focused');
  check(anchored.querySelector('strong')?.textContent === 'Ready' && anchored.querySelector('a[target="_blank"][rel~="noopener"]'),
    'The thread lost Markdown');
  check(anchored.querySelector('.bot-thread-name').textContent === 'Reviewer' && anchored.querySelector('.bot-thread-avatar'),
    'The sender header was missing');
  check($('#bot-thread-message-39').classList.contains('bot-thread-own'), 'The Bot’s own turn was not distinguished');
  check($('#profile-thread').textContent === 'Reviewer' && !$('#profile-thread').hidden && !$('#bot-thread-close').hidden,
    'The header did not show the thread breadcrumb and Close');
  check(!$('#bot-thread-latest').hidden, 'Jump to latest was missing for an anchored thread');
  check(panel.scrollWidth <= panel.clientWidth, 'The thread overflowed horizontally');
  check(Math.abs(panel.getBoundingClientRect().height - panel.parentElement.getBoundingClientRect().height) < 1,
    'The thread did not fill the conversation area');

  $('#bot-thread-older').click();
  await waitFor(() => $('#bot-thread-message-12'), 'Earlier thread messages did not load');
  check($('#bot-thread-older').hidden, 'Earlier messages stayed available after the first message');

  $('#bot-thread-latest').click();
  await waitFor(() => !params().has('message') && $('#bot-thread-latest').hidden, 'Jump to latest kept the anchor');

  $('#bot-thread-close').click();
  await waitFor(() => panel.hidden && !params().has('thread'), 'Close did not return to the conversation');
  check(getComputedStyle(conversation).visibility === 'visible' && $('#profile-thread').hidden, 'The conversation did not come back');

  const entry = $('#conversation-bot-dms');
  check(entry.getAttribute('href') === '/?bot=scout&thread=', 'The conversation menu did not link to the Bot list');
  entry.click();
  await waitFor(() => $('.bot-thread-peer'), 'The Bot list did not open');
  check($('#profile-thread').textContent === 'Bots', 'The Bot list breadcrumb was wrong');
  $('.bot-thread-peer').click();
  await waitFor(() => $('#bot-thread-message-40') && params().get('thread') === 'reviewer', 'Choosing a Bot did not open its thread');
  check(!dmRequests.at(-1).includes('anchor'), 'Choosing a Bot opened an anchored thread');
  panel.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
  await waitFor(() => panel.hidden, 'Escape did not close the thread');

  row.click();
  await waitFor(() => !panel.hidden, 'The thread did not reopen');
  history.back();
  await waitFor(() => panel.hidden && !params().has('thread'), 'Back did not close the thread');
}

const index = (await readAsset('/index.html')).replace('<script type="module" src="/app.js"></script>', `
  <script>(${fixtures})();</script>
  <script type="module">
    import '/app.js';
    try {
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script>`);
await withAssetServer({ '/': index }, async (origin) => {
  for (const query of ['bot=scout', 'bot=scout&thread=reviewer&message=40']) {
    requirePass(await dumpDom(process.argv[2], `${origin}/?${query}`, { flags: ['--window-size=390,844'] }),
      'The browser did not finish the Bot thread checks', query);
  }
});
