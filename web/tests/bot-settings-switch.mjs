import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function fixtures() {
  window.EventSource = undefined;
  const bot = { id: 'release', name: 'Release', paused: false, canDelete: true, presence: 'idle', fixPullRequests: true };
  window.fetch = async (path, { body } = {}) => {
    if (path === '/api/bots') return Response.json([bot]);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: bot.canDelete });
    if (path.startsWith('/api/view?')) return Response.json({
      bot, events: [], olderBefore: 0, presence: 'idle', activeSessions: [], sessionTasks: [], sessionsBefore: 0,
      sessionCursor: 0, waits: [], canDecide: true, composerPerson: 'local-owner',
      limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
    });
    if (path === '/api/bots/profile') {
      Object.assign(bot, JSON.parse(body));
      return Response.json(bot);
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exercise() {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const settle = () => new Promise(resolve => setTimeout(resolve, 300));
  const switchElement = () => document.querySelector('.settings-row-switch');
  for (let attempt = 0; attempt < 100 && !switchElement(); attempt++) await new Promise(resolve => setTimeout(resolve, 20));
  check(switchElement(), 'Bot settings did not show the pull request switch');
  check(matchMedia('(forced-colors: active)').matches, 'The browser did not force colors');

  const canvas = document.createElement('canvas');
  canvas.width = canvas.height = 1;
  const context = canvas.getContext('2d', { willReadFrequently: true });
  function paint(...colors) {
    context.clearRect(0, 0, 1, 1);
    for (const color of colors) {
      context.fillStyle = color;
      context.fillRect(0, 0, 1, 1);
    }
    return [...context.getImageData(0, 0, 1, 1).data];
  }
  const distinct = (a, b) => a.some((channel, index) => Math.abs(channel - b[index]) >= 64);
  let surface = switchElement().parentElement;
  while (paint(getComputedStyle(surface).backgroundColor)[3] === 0) surface = surface.parentElement;
  const backdrop = getComputedStyle(surface).backgroundColor;

  function look(state) {
    const toggle = switchElement();
    const track = getComputedStyle(toggle);
    const thumb = getComputedStyle(toggle, '::before');
    const edge = track.boxShadow === 'none' ? track.backgroundColor : track.boxShadow.match(/^(rgba?\([^)]*\)|\S+)/)[1];
    const fill = paint(backdrop, track.backgroundColor);
    check(distinct(fill, paint(backdrop)) || distinct(paint(backdrop, edge), paint(backdrop)),
      `The ${state} switch blends into the settings card`);
    check(distinct(paint(backdrop, track.backgroundColor, thumb.backgroundColor), fill),
      `The ${state} switch thumb blends into its track`);
    return { fill, position: thumb.translate };
  }

  const on = look('on');
  switchElement().click();
  await settle();
  check(!switchElement().checked, 'Clicking the switch did not turn fixes off');
  const off = look('off');
  check(distinct(on.fill, off.fill) && on.position !== off.position, 'On and off look the same');

  switchElement().disabled = true;
  await settle();
  const disabledOff = look('disabled off');
  switchElement().checked = true;
  await settle();
  const disabledOn = look('disabled on');
  check(distinct(disabledOn.fill, disabledOff.fill), 'A switch you cannot change looks the same on and off');
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
  for (const scheme of [[], ['--force-dark-mode']]) {
    const dom = await dumpDom(process.argv[2], `${origin}/?bot=release&settings=1`,
      { flags: ['--force-high-contrast', '--force-prefers-reduced-motion', ...scheme], budget: 5000 });
    requirePass(dom, 'the browser did not finish the switch checks', `${scheme.length ? 'Dark' : 'Light'} forced colors`);
  }
});
