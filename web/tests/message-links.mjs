import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { promisify } from 'node:util';

function fixtures() {
  window.EventSource = undefined;
  const bot = { id: 'release', name: 'Release', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  const time = Math.floor(Date.now() / 1000) - 600;
  const link = '<p>See <a href="https://example.com/pull/12">https://example.com/pull/12</a></p>';
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
const directory = await mkdtemp(join(tmpdir(), 'flock-message-links-dom-'));
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', `--user-data-dir=${directory}`, '--virtual-time-budget=3000', '--dump-dom',
    `http://127.0.0.1:${server.address().port}/?bot=release`], { encoding: 'utf8', timeout: 25000 });
  const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the message link checks');
} finally {
  server.close();
  await rm(directory, { recursive: true, force: true });
}
