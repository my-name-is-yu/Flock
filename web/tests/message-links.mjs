import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function fixtures() {
  window.EventSource = undefined;
  const bot = { id: 'release', name: 'Release', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  const time = Math.floor(Date.now() / 1000) - 600;
  const link = '<h1>Heading</h1><blockquote><p>Quoted</p></blockquote><p>See <a href="https://example.com/pull/12">https://example.com/pull/12</a> and <code>inline</code></p><pre><code>block</code></pre><hr>';
  const events = [
    { id: 1, kind: 'conversation_message', time, data: { text: 'See https://example.com/pull/12', textHtml: link } },
    { id: 2, kind: 'conversation_reply', time: time + 10, data: { text: 'See https://example.com/pull/12', textHtml: link } },
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
  const check = (value, message) => { if (!value) throw new Error(message); };
  for (let attempt = 0; attempt < 50 && !document.querySelector('#event-2 a'); attempt++)
    await new Promise(resolve => setTimeout(resolve, 10));
  const links = [...document.querySelectorAll('#event-1 a[href], #event-2 a[href]')];
  check(links.length === 2, 'The message links did not render');
  check(links.every((link) => link.target === '_blank' && link.rel === 'noopener noreferrer'),
    'A message link would replace the conversation');
  const rootStyle = getComputedStyle(document.documentElement);
  check(rootStyle.getPropertyValue('--dur-medium').trim() === '240ms', 'Missing --dur-medium token');
  const botQuote = getComputedStyle(document.querySelector('#event-2 blockquote'));
  check(botQuote.borderInlineStartWidth === '2px' && botQuote.borderInlineStartColor !== 'rgba(0, 0, 0, 0)',
    'Bot blockquote border is missing');
  const probe = document.createElement('span');
  probe.style.color = 'var(--accent)';
  document.body.append(probe);
  const accentColor = getComputedStyle(probe).color;
  probe.remove();
  check(getComputedStyle(document.querySelector('#event-2 a')).color === accentColor,
    'Bot Markdown link did not use the accent color');
  const userBubble = document.querySelector('#event-1 .bubble');
  check(getComputedStyle(document.querySelector('#event-1 a')).color === getComputedStyle(userBubble).color,
    'User Markdown link did not inherit the bubble text color');
  const inlineCode = getComputedStyle(document.querySelector('#event-2 p code'));
  const blockCode = getComputedStyle(document.querySelector('#event-2 pre code'));
  check(inlineCode.backgroundColor !== 'rgba(0, 0, 0, 0)' && blockCode.backgroundColor === 'rgba(0, 0, 0, 0)',
    'Markdown code block styling did not differentiate inline and fenced code');
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
  requirePass(await dumpDom(process.argv[2], `${origin}/?bot=release`), 'The browser did not finish the message link checks');
});
