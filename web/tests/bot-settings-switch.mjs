import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { promisify } from 'node:util';

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

const assets = new URL('../assets/', import.meta.url);
const read = (path) => readFile(new URL(`.${path}`, assets));
const index = (await read('/index.html')).toString().replace('<script type="module" src="/app.js"></script>', `
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
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  for (const scheme of [[], ['--force-dark-mode']]) {
    const directory = await mkdtemp(join(tmpdir(), 'flock-bot-settings-switch-'));
    try {
      const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
        '--disable-background-networking', '--force-high-contrast', '--force-prefers-reduced-motion', ...scheme, `--user-data-dir=${directory}`,
        '--virtual-time-budget=5000', '--dump-dom', `http://127.0.0.1:${server.address().port}/?bot=release&settings=1`],
      { encoding: 'utf8', timeout: 25000 });
      const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
      if (result?.[1] !== 'pass') {
        throw new Error(`${scheme.length ? 'Dark' : 'Light'} forced colors: ${result?.[1] ?? 'the browser did not finish the switch checks'}`);
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
} finally {
  server.close();
}
