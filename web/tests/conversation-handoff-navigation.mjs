import { dumpDom, readAsset, requirePass, respondWithAsset, withServer } from './dom-harness.mjs';

function fixtures() {
  const bots = [
    { id: 'sender-fixture', name: 'Sender fixture', model: 'demo', paused: false, canDelete: true, presence: 'idle' },
    { id: 'other-fixture', name: 'Other fixture', model: 'demo', paused: false, canDelete: true, presence: 'idle' },
    { id: 'recipient-fixture', name: 'Recipient fixture', model: 'demo', paused: false, canDelete: true, presence: 'idle' },
  ];
  const handoff = {
    id: 43, time: 1700000043, kind: 'conversation_handoff',
    data: { peerBotId: 'recipient-fixture', messageId: 42 },
  };
  const delivered = {
    id: 42, time: 1700000042, kind: 'conversation_message',
    data: { text: 'Review the release.', botMessage: { fromBotId: 'sender-fixture', fromName: 'Sender fixture' } },
  };
  const latest = { id: 44, time: 1700000044, kind: 'memory_remembered', data: {} };
  const olderConversation = Array.from({ length: 42 }, (_, index) => ({
    id: index + 1, time: 1700000000 + index, kind: 'conversation_message',
    data: { text: `Earlier message ${index + 1}. ${'Context '.repeat(8)}` },
  }));
  olderConversation.push(handoff);
  const viewer = new URLSearchParams(location.search).get('viewer') ?? 'authorized';
  let hasAccess = !['restricted', 'grant', 'source-grant', 'source-delayed-grant', 'inactive-grant'].includes(viewer);
  let sourcePageReads = 0;
  let conversationPageReads = 0;
  window.__conversationPageReads = () => conversationPageReads;
  window.__resolveSourcePage = null;
  const visibleBots = () => hasAccess ? bots : bots.filter(bot => bot.id !== 'recipient-fixture');
  window.__setHandoffAccess = allowed => {
    hasAccess = allowed;
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.EventSource = undefined;
  const realFetch = window.fetch.bind(window);
  window.fetch = async (path, request = {}) => {
    const url = new URL(typeof path === 'string' ? path : path.url, location.origin);
    if (url.pathname === '/api/bots') return Response.json(visibleBots());
    if (url.pathname === '/api/inbox') return Response.json({ items: [] });
    if (url.pathname === '/api/settings/projects') return Response.json({ environments: [], recent: null });
    if (url.pathname.startsWith('/api/schedules')) return Response.json({ active: [], history: [], canManage: true });
    if (url.pathname === '/api/notifications') return Response.json({ through: 0, scope: {}, events: [], inbox: [] });
    if (url.pathname === '/api/view') {
      const bot = visibleBots().find(entry => entry.id === url.searchParams.get('bot'));
      const events = bot?.id === 'sender-fixture'
        ? [latest]
        : [delivered];
      return Response.json({
        bot, events, olderBefore: bot?.id === 'sender-fixture' ? 44 : 0, presence: 'idle', activeSessions: [], activeTasks: [], parallelTasks: [],
        sessionTasks: [], sessionsBefore: 0, sessionCursor: 0, historyCursor: 0, changedMessageIds: [],
        waits: [], canDecide: true, composerPerson: null,
        limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
      });
    }
    if (url.pathname === '/api/conversation') {
      conversationPageReads++;
      if (viewer.startsWith('source-delayed-') && sourcePageReads++ === 0) {
        const events = [hasAccess ? handoff : { ...handoff, data: {} }];
        return await new Promise(resolve => { window.__resolveSourcePage = () => resolve(Response.json({ events, olderBefore: 0 })); });
      }
      const events = viewer.startsWith('inactive-') ? olderConversation : [handoff];
      return Response.json({
        events: events.map(event => event.id === handoff.id && !hasAccess ? { ...handoff, data: {} } : event),
        olderBefore: 0,
      });
    }
    return realFetch(path, request);
  };
}

async function exercise() {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  async function waitFor(condition, message) {
    for (let attempt = 0; attempt < 80 && !condition(); ++attempt)
      await new Promise(resolve => setTimeout(resolve, 10));
    check(condition(), message);
  }

  const requestedBot = new URLSearchParams(location.search).get('bot');
  if (requestedBot === 'sender-fixture') {
    const viewer = new URLSearchParams(location.search).get('viewer');
    if (viewer === 'source-grant' || viewer === 'source-revoke') {
      await waitFor(() => document.querySelector('#event-43'), 'The opened source handoff did not render');
      const notice = document.querySelector('#event-43');
      if (viewer === 'source-grant') {
        check(!notice.querySelector('a') && notice.textContent.includes('Bot message sent'),
          'The restricted source page exposed peer navigation');
        window.__setHandoffAccess(true);
        await waitFor(() => document.querySelector('#event-43 a'), 'The opened source page was not reprojected after access was granted');
      } else {
        check(notice.querySelector('a'), 'The authorized source page did not render its handoff link');
        window.__setHandoffAccess(false);
        await waitFor(() => !document.querySelector('#event-43 a') &&
          document.querySelector('#event-43').textContent.includes('Bot message sent'),
        'The opened source page retained peer navigation after access was revoked');
      }
      return;
    }
    if (viewer.startsWith('source-delayed-')) {
      const granting = viewer.endsWith('grant');
      await waitFor(() => window.__resolveSourcePage, 'The source page request did not become pending');
      window.__setHandoffAccess(granting);
      await waitFor(() => !!document.querySelector('#bot-list [data-focus="bot:recipient-fixture"]') === granting,
        'The access change did not refresh the Bot roster');
      window.__resolveSourcePage();
      if (granting) {
        await waitFor(() => document.querySelector('#event-43 a'),
          'A delayed source response installed a stale restricted projection');
      } else {
        await waitFor(() => {
          const notice = document.querySelector('#event-43');
          return notice != null && !notice.querySelector('a') && notice.textContent.includes('Bot message sent');
        },
        'A delayed source response retained a stale authorized projection');
      }
      return;
    }
    if (viewer.startsWith('inactive-')) {
      const granting = viewer.endsWith('grant');
      await waitFor(() => !document.querySelector('#older').hidden, 'The older handoff page did not become available');
      document.querySelector('#older').click();
      await waitFor(() => document.querySelector('#event-43') &&
        Boolean(document.querySelector('#event-43 a')) === !granting,
        'The restricted cached handoff did not render');
      const scroller = document.querySelector('#conversation');
      check(scroller.scrollHeight > scroller.clientHeight, 'The inactive-history fixture did not create a scrollable page');
      scroller.scrollTop = 0;
      scroller.dispatchEvent(new Event('scroll'));
      document.querySelector('#bot-list [data-focus="bot:other-fixture"]').click();
      await waitFor(() => new URLSearchParams(location.search).get('bot') === 'other-fixture',
        'The inactive-history fixture did not switch conversations');
      const readsBeforeCachedReopen = window.__conversationPageReads();
      document.querySelector('#bot-list [data-focus="bot:sender-fixture"]').click();
      await waitFor(() => new URLSearchParams(location.search).get('bot') === 'sender-fixture' &&
        document.querySelector('#event-43') && Boolean(document.querySelector('#event-43 a')) === !granting,
        'The cached sender history did not reopen while restricted');
      check(window.__conversationPageReads() === readsBeforeCachedReopen,
        'The sender history was fetched instead of restored from its inactive-page cache');
      document.querySelector('#bot-list [data-focus="bot:other-fixture"]').click();
      await waitFor(() => new URLSearchParams(location.search).get('bot') === 'other-fixture',
        'The inactive-history fixture did not return to the other conversation');
      window.__setHandoffAccess(granting);
      await waitFor(() => Boolean(document.querySelector('#bot-list [data-focus="bot:recipient-fixture"]')) === granting,
        'The access change did not refresh the Bot roster');
      const readsBeforeReprojection = window.__conversationPageReads();
      document.querySelector('#bot-list [data-focus="bot:sender-fixture"]').click();
      await waitFor(() => new URLSearchParams(location.search).get('bot') === 'sender-fixture' && document.querySelector('#event-43'),
        'The cached sender history did not reopen');
      const refreshed = document.querySelector('#event-43');
      const link = refreshed.querySelector('a');
      check(window.__conversationPageReads() > readsBeforeReprojection &&
        (granting
          ? refreshed.textContent.includes('Message sent to Recipient fixture') &&
            new URL(link?.href ?? location.href).searchParams.get('bot') === 'recipient-fixture'
          : refreshed.textContent.includes('Bot message sent') && !link),
        'The inactive handoff cache was not reprojected after the access change');
      return;
    }
    await waitFor(() => !document.querySelector('#older').hidden, 'The older handoff page did not become available');
    document.querySelector('#older').click();
    if (viewer === 'restricted' || viewer === 'grant') {
      await waitFor(() => document.querySelector('#event-43'), 'The restricted handoff notice did not render');
      const notice = document.querySelector('#event-43');
      check(notice.textContent.includes('Bot message sent') && !notice.querySelector('a'),
        'A restricted projection exposed a peer label or navigation action');
      if (viewer === 'restricted') return;
      window.__setHandoffAccess(true);
      await waitFor(() => document.querySelector('#event-43 a'), 'Loaded handoff history was not reprojected after access was granted');
    } else {
      await waitFor(() => document.querySelector('#event-43 a'), 'The authorized handoff link did not render');
      if (viewer === 'revoke') {
        window.__setHandoffAccess(false);
        await waitFor(() => !document.querySelector('#event-43 a') && document.querySelector('#event-43').textContent.includes('Bot message sent'),
          'Loaded handoff history retained peer navigation after access was revoked');
        return;
      }
    }
    const link = document.querySelector('#event-43 a');
    const target = new URL(link.href);
    check(target.searchParams.get('bot') === 'recipient-fixture' && target.searchParams.get('message') === '42',
      'The handoff link did not target the authorized recipient message');
    link.click();
    await waitFor(() => new URLSearchParams(location.search).get('bot') === 'recipient-fixture',
      'The handoff link did not navigate to the recipient conversation');
    return;
  }

  check(requestedBot === 'recipient-fixture', 'The browser opened an unexpected conversation');
  await waitFor(() => document.querySelector('#event-42') && document.querySelector('#profile-name')?.textContent === 'Recipient fixture',
    'The delivered message did not appear in the recipient conversation');
  check(!new URLSearchParams(location.search).has('message'), 'The one-time message target remained in the URL');
  check(document.activeElement?.id === 'event-42', 'Navigation did not focus the delivered message');
  check(document.querySelector('#event-42').textContent.includes('Review the release.'), 'The delivered message was not readable');

  const linkedMessage = document.createElement('a');
  linkedMessage.id = 'event-99';
  linkedMessage.className = 'bot-message-row';
  linkedMessage.href = '/?bot=sender-fixture';
  linkedMessage.tabIndex = -1;
  linkedMessage.append(Object.assign(document.createElement('span'), { className: 'bubble', textContent: 'Linked Bot DM' }));
  document.querySelector('#conversation').append(linkedMessage);
  const linkedTarget = window.__conversationMessageRow(99);
  check(linkedTarget === linkedMessage, 'Navigation did not find the linked Bot DM row');
  window.__focusConversationMessage(linkedTarget);
  check(document.activeElement === linkedMessage, 'Navigation did not focus the linked Bot DM row');
}

const html = (await readAsset('/index.html')).replace('<script type="module" src="/app.js"></script>', `
  <img hidden src="/test-pending.svg">
  <script>(${fixtures})();</script>
  <script type="module">
    import { conversationMessageRow, focusConversationMessage } from '/timeline.js';
    import '/app.js';
    window.__conversationMessageRow = conversationMessageRow;
    window.__focusConversationMessage = focusConversationMessage;
    let failed = false;
    try { await (${exercise})(); document.documentElement.dataset.flockResult = 'pass'; }
    catch (error) {
      failed = true;
      document.documentElement.dataset.flockResult = error.message;
    }
    finally { await fetch('/test-complete'); }
  </script>`);
let pendingAsset;
let testComplete = false;
await withServer((request, response) => {
  const { pathname } = new URL(request.url, 'http://localhost');
  if (pathname === '/test-pending.svg') {
    if (testComplete) response.writeHead(204).end();
    else pendingAsset = response;
    return;
  }
  if (pathname === '/test-complete') {
    testComplete = true;
    const pending = pendingAsset;
    pendingAsset = null;
    pending?.writeHead(204).end();
    response.writeHead(204).end();
    return;
  }
  return respondWithAsset(response, pathname, { '/': html });
}, async origin => {
  for (const viewer of ['authorized', 'restricted', 'grant', 'revoke', 'source-grant', 'source-revoke', 'source-delayed-grant',
    'source-delayed-revoke', 'inactive-grant', 'inactive-revoke']) {
    pendingAsset = null;
    testComplete = false;
    const params = new URLSearchParams({ bot: 'sender-fixture', viewer });
    if (viewer.startsWith('source-')) params.set('message', '43');
    const url = `${origin}/?${params}`;
    const dom = await dumpDom(process.argv[2], url, { flags: ['--force-prefers-reduced-motion'], budget: 0 });
    requirePass(dom, `The ${viewer} handoff check did not finish`);
  }
});
