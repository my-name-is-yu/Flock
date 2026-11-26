import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function fixtures() {
  window.EventSource = undefined;
  const bots = Array.from({ length: 12 }, (_, index) => ({
    id: `bot-${index + 1}`,
    name: `Bot ${index + 1}`,
    model: 'demo',
    paused: false,
    canDelete: index !== 0,
    presence: 'idle',
  }));
  window.fetch = async (path) => {
    if (path === '/api/bots') return Response.json(bots);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: true });
    if (path.startsWith('/api/view?')) {
      const id = new URL(path, 'http://localhost').searchParams.get('bot') || 'bot-1';
      const bot = bots.find((item) => item.id === id) ?? bots[0];
      return Response.json({
        bot, events: [], olderBefore: 0, presence: 'idle', activeSessions: [], activeTasks: [], parallelTasks: [],
        sessionTasks: [], sessionsBefore: 0, sessionCursor: 0, waits: [], canDecide: true,
        composerPerson: 'local-owner', limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
      });
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exercise() {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 60; attempt++) {
      if (predicate()) return;
      await new Promise((resolve) => setTimeout(resolve, 15));
    }
    check(predicate(), message);
  }
  const key = (target, code) => target.dispatchEvent(new KeyboardEvent('keydown', { key: code, bubbles: true, cancelable: true }));

  await waitFor(() => $('#bot-list .bot-row') && $('#profile-heading') && $('#conversation-bot-settings'),
    'Workspace UI did not render');

  // 1. Discoverability: conversation-bot-settings & clickable profile heading when needs-you is hidden.
  check(!$('#conversation-bot-settings').hidden && $('#needs-you').hidden, 'Header bot controls were not ready');
  $('#conversation-bot-settings').click();
  await waitFor(() => $('#detail')?.open, '#conversation-bot-settings did not open Bot settings');
  // 2. Respect bot.canDelete === false in General settings.
  check(!$('.settings-avatar-button') && $('.settings-avatar-static') && !$('.settings-name-button') &&
    $('.settings-name')?.textContent === 'Bot 1',
    'Read-only Bot still offered interactive avatar or rename buttons');
  $('#close-detail').click();
  check(!$('#detail').open, 'Detail did not close');

  $('#profile-heading').click();
  await waitFor(() => $('#detail')?.open, 'Clicking #profile-heading did not open Bot settings when Needs you is hidden');
  $('#close-detail').click();

  // Switch to editable Bot 2 and verify interactive controls appear.
  document.querySelector('[data-focus="bot:bot-2"]').click();
  await waitFor(() => $('#profile-name')?.textContent === 'Bot 2' && $('#message')?.placeholder === 'Message Bot 2',
    'Failed to switch to Bot 2');
  $('#profile-heading').click();
  await waitFor(() => $('#detail')?.open && $('.settings-avatar-button') && $('.settings-name-button'),
    'Editable Bot did not render interactive avatar and rename controls');
  $('#close-detail').click();

  // 3. Viewport-fixed menu popover positioning & scroll close.
  const scroller = $('.sidebar-scroll');
  const bottomTrigger = document.querySelector('[data-focus="menu:bot-12"]');
  bottomTrigger.scrollIntoView({ block: 'end' });
  bottomTrigger.click();
  await waitFor(() => $('#bot-menu'), 'Bot menu popover did not open');
  const popover = $('#bot-menu');
  const style = getComputedStyle(popover);
  const rect = popover.getBoundingClientRect();
  check(style.position === 'fixed' && rect.top >= 0 && rect.bottom <= window.innerHeight &&
    rect.left >= 0 && rect.right <= window.innerWidth,
    `Fixed menu popover escaped viewport bounds (${JSON.stringify({ pos: style.position, top: rect.top, bottom: rect.bottom, h: window.innerHeight })})`);
  scroller.dispatchEvent(new Event('scroll'));
  check(!$('#bot-menu'), 'Scrolling sidebar did not close menu popover');

  // 4. Keyboard sidebar resizer & collapse persistence.
  const resizer = $('#sidebar-resizer');
  check(resizer.tabIndex === 0 && resizer.getAttribute('aria-valuemin') === '216' && resizer.getAttribute('aria-valuemax') === '416',
    'Sidebar resizer ARIA range or tabindex missing');
  const startWidth = Number(resizer.getAttribute('aria-valuenow'));
  key(resizer, 'ArrowRight');
  check(Number(resizer.getAttribute('aria-valuenow')) === startWidth + 16, 'ArrowRight did not widen sidebar');
  key(resizer, 'ArrowLeft');
  check(Number(resizer.getAttribute('aria-valuenow')) === startWidth, 'ArrowLeft did not narrow sidebar');
  key(resizer, 'Home');
  check(resizer.getAttribute('aria-valuenow') === '216', 'Home did not set min sidebar width');
  key(resizer, 'End');
  check(resizer.getAttribute('aria-valuenow') === '416', 'End did not set max sidebar width');
  key(resizer, 'Enter');
  check(resizer.getAttribute('aria-valuenow') === '296', 'Enter did not reset default sidebar width');

  $('#toggle-chat-sidebar').click();
  check(JSON.parse(localStorage.getItem('flock-sidebar-collapsed')) === true &&
    resizer.tabIndex === -1 && resizer.getAttribute('aria-hidden') === 'true',
    'Collapsing sidebar did not persist state or disable resizer focus');
  $('#show-chat-sidebar').click();
  check(JSON.parse(localStorage.getItem('flock-sidebar-collapsed')) === false && resizer.tabIndex === 0,
    'Expanding sidebar did not clear persisted collapsed state');
}

const index = (await readAsset('/index.html')).replace('<script type="module" src="/app.js"></script>', `
  <script>(${fixtures})();</script>
  <script type="module">
    import '/app.js';
    try {
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error?.stack || error?.message || String(error);
    }
  </script>`);
await withAssetServer({ '/': index }, async (origin) => {
  requirePass(await dumpDom(process.argv[2], `${origin}/?bot=bot-1`), 'Browser did not finish sidebar-header-ux checks');
});
