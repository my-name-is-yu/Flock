import { dumpDom, readAsset, requirePass, respondWithAsset, withServer } from './dom-harness.mjs';

const scout = { id: 'scout', name: 'Scout' };
const reviewer = { id: 'reviewer', name: 'Reviewer' };
const message = (id, fromBotId, textHtml) => ({
  id, time: 1795860000 + id * 60, data: { fromBotId, textHtml },
});
const page = (await readAsset('/bot-dms.html')).replace('</body>', `<script type="module">
  const check = (value, text) => { if (!value) throw new Error(text); };
  const find = (selector) => document.querySelector(selector);
  async function waitFor(selector) {
    while (!find(selector)) await new Promise(resolve => setTimeout(resolve, 20));
  }
  try {
    if (!location.search.includes('peer=')) {
      await waitFor('.dm-peer');
      check(find('h1').textContent === 'Scout’s talks with other Bots', 'picker heading');
      check(find('.dm-peer').getAttribute('href') === '/bot-dms?bot=scout&peer=reviewer', 'picker entry');
      check(find('#dm-back').getAttribute('href') === '/?bot=scout', 'picker back');
    } else {
      await waitFor('.dm-anchored');
      const anchor = find('.dm-anchored');
      check(anchor.id === 'dm-message-40' && document.activeElement === anchor, 'focused deep link');
      check(anchor.querySelector('strong').textContent === 'Ready', 'Markdown body');
      check(anchor.querySelector('a[target="_blank"][rel~="noopener"]'), 'Markdown link');
      check(find('#dm-message-41 .sr-only .msg-name').textContent === 'Reviewer', 'grouped sender');
      check(!find('form') && find('#dm-scroll').getAttribute('aria-label') === 'Read-only Bot conversation', 'read-only transcript');
      check(!find('#dm-latest').hidden && find('#dm-latest').getAttribute('href') === '/bot-dms?bot=scout&peer=reviewer', 'latest link');
      const older = find('#dm-older');
      const original = find('#dm-message-41');
      older.click();
      await waitFor('#dm-message-12');
      check(older.hidden && find('#dm-message-41') === original, 'earlier page replaced an existing row');
      check(find('#dm-scroll').scrollWidth <= find('#dm-scroll').clientWidth, 'narrow transcript overflows');
    }
    document.documentElement.dataset.flockResult = 'pass';
  } catch (error) {
    document.documentElement.dataset.flockResult = error.message;
  }
</script></body>`);

await withServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname !== '/api/bot-dms') return respondWithAsset(res, url.pathname, { '/bot-dms': page });
  res.setHeader('Content-Type', 'application/json');
  const data = url.searchParams.has('before')
    ? { bot: scout, peer: reviewer, events: [message(12, 'scout', '<p>Check the release.</p>')], olderBefore: 0 }
    : url.searchParams.has('peer')
      ? { bot: scout, peer: reviewer, olderBefore: 40, hasNewer: true, events: [
        message(40, 'reviewer', '<p><strong>Ready</strong>. See the <a href="https://example.com">report</a>.</p>'),
        message(41, 'reviewer', '<p>All checks have finished.</p>'),
      ] }
      : { bot: scout, peers: [reviewer] };
  res.end(JSON.stringify(data));
}, async base => {
  for (const query of ['bot=scout', 'bot=scout&peer=reviewer&anchor=40']) {
    const dom = await dumpDom(process.argv[2], `${base}/bot-dms?${query}`, { flags: ['--window-size=390,844'] });
    requirePass(dom, 'Bot DM did not render', query);
  }
});
