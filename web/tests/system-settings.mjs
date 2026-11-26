import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { promisify } from 'node:util';

async function exercise() {
  const { loadInstallation } = await import('/installation-settings.js');
  const check = (value, message) => { if (!value) throw new Error(message); };
  const installation = (system) => ({ access: { mode: 'local', users: [] }, connections: {}, system });
  let current;
  window.fetch = async (path) => {
    if (path === '/api/settings/installation') return Response.json(installation(current));
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
  async function show(system) {
    current = system;
    await loadInstallation();
    return [...document.querySelectorAll('#system .settings-row')].map(item => ({
      title: item.querySelector('strong').textContent,
      value: item.querySelector('small')?.textContent,
      next: item.querySelector('small b')?.textContent,
      change: !!item.querySelector('button'),
      badge: item.querySelector('.setting-state')?.textContent,
    }));
  }
  const neither = { port: false, sessionRead: false };

  let rows = await show({
    active: { port: 52011, sessionRead: 'off' }, saved: { port: null, sessionRead: null },
    next: { port: null, sessionRead: 'off' }, overrides: neither, pending: neither,
  });
  check(rows.length === 2, 'A fresh start showed a restart notice');
  check(rows[0].value === '52011' && !rows[0].next, `An automatic port with nothing saved read “${rows[0].value}”`);
  check(rows[1].value === 'Off' && rows[0].change && rows[1].change, 'A fresh start did not show its values with Change');

  rows = await show({
    active: { port: 52011, sessionRead: 'off' }, saved: { port: null, sessionRead: 'on_demand' },
    next: { port: null, sessionRead: 'on_demand' }, overrides: neither, pending: { port: false, sessionRead: true },
  });
  check(rows[0].value === '52011', `A pending Read T3 threads change made the port read “${rows[0].value}”`);
  check(rows[1].value === 'Off → When needed after restart' && rows[1].next === 'When needed',
    `A pending Read T3 threads change read “${rows[1].value}”`);
  check(rows[2]?.title === 'Restart required', 'A pending change did not ask for a restart');

  rows = await show({
    active: { port: 5000, sessionRead: 'off' }, saved: { port: null, sessionRead: null },
    next: { port: null, sessionRead: 'off' }, overrides: neither, pending: { port: true, sessionRead: false },
  });
  check(rows[0].value === '5000 → Automatic after restart' && rows[0].next === 'Automatic',
    `Returning to an automatic port read “${rows[0].value}”`);
  check(rows[1].value === 'Off' && rows[2]?.title === 'Restart required', 'A pending port change changed the other row');

  rows = await show({
    active: { port: 52011, sessionRead: 'on_demand' }, saved: { port: 5000, sessionRead: 'prefill' },
    next: { port: null, sessionRead: 'on_demand' }, overrides: { port: true, sessionRead: true }, pending: neither,
  });
  check(rows.length === 2 && rows[0].value === '52011' && rows[1].value === 'When needed',
    `Launch settings read “${rows[0].value}” and “${rows[1].value}”`);
  check(rows.every(item => item.badge === 'Set at launch' && !item.change), 'Launch settings could be changed here');
}

const assets = new URL('../assets/', import.meta.url);
const read = (path) => readFile(new URL(`.${path}`, assets));
const page = (await read('/settings.html')).toString().replace('<script type="module" src="/settings.js"></script>', `
  <script type="module">
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
    response.setHeader('Content-Type', pathname === '/settings' ? 'text/html' : contentTypes[extname(pathname)] ?? 'application/octet-stream');
    response.end(pathname === '/settings' ? page : await read(pathname));
  } catch {
    response.writeHead(404).end();
  }
});
const directory = await mkdtemp(join(tmpdir(), 'flock-system-settings-dom-'));
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', `--user-data-dir=${directory}`, '--virtual-time-budget=3000', '--dump-dom',
    `http://127.0.0.1:${server.address().port}/settings#system`], { encoding: 'utf8', timeout: 25000 });
  const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the System settings checks');
} finally {
  server.close();
  await rm(directory, { recursive: true, force: true });
}
