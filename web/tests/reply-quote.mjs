import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function fixtures() {
  window.EventSource = undefined;
  const bot = { id: 'release', name: 'Release', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  const time = Math.floor(Date.now() / 1000) - 600;
  const events = [
    { id: 1, kind: 'conversation_message', time, data: { text: 'Draft the notes' } },
    { id: 2, kind: 'conversation_reply', time: time + 10, data: { text: 'Here is the draft' } },
    { id: 3, kind: 'conversation_message', time: time + 20, data: { text: 'Thanks', replyToMessageId: 2 } },
    { id: 4, kind: 'conversation_reply', time: time + 30, data: { text: 'Anything else?' } },
    { id: 5, kind: 'conversation_message', time: time + 40, data: { text: 'Shorten the draft', replyToMessageId: 2 } },
    { id: 6, kind: 'conversation_reply', time: time + 50, data: { text: 'Done' } },
    { id: 7, kind: 'user_answer', time: time + 60, data: { text: 'Use main', question: 'Which branch?' } },
  ];
  window.fetch = async (path) => {
    if (path === '/api/bots') return Response.json([bot]);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: true });
    if (path.startsWith('/api/view?')) return Response.json({
      bot, events, olderBefore: 0, presence: 'idle', activeSessions: [], sessionTasks: [], sessionsBefore: 0,
      sessionCursor: 0, waits: [], canDecide: true, composerPerson: 'local-owner',
      limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
    });
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exercise() {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 50; attempt++) {
      if (predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    check(predicate(), message);
  }
  await waitFor(() => $('#event-7'), 'The conversation did not render every message');
  check(!$('#event-3 .msg-quote') && !$('.open-original-message'),
    'A reply right under its original repeated it');
  const quote = $('#event-5 .msg-quote-link');
  check(quote?.textContent === 'Here is the draft' && quote.compareDocumentPosition($('#event-5 .bubble')) & Node.DOCUMENT_POSITION_FOLLOWING,
    'A reply to an earlier message did not quote it above the bubble');
  check($('#event-7 .msg-quote')?.textContent === 'Which branch?', 'An answer lost its question');
  quote.click();
  await waitFor(() => $('#event-2')?.classList.contains('conversation-source-target'),
    'The quote did not jump to the original message');
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
  requirePass(await dumpDom(process.argv[2], `${origin}/?bot=release`), 'The browser did not finish the reply quote checks');
});
