import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { promisify } from 'node:util';

async function exercise() {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const wait = (ms) => new Promise(resolve => setTimeout(resolve, ms));
  const pointer = (type, target, relatedTarget = null, pointerType = 'mouse') =>
    target.dispatchEvent(new PointerEvent(type, { bubbles: true, pointerType, relatedTarget }));
  const tip = () => document.querySelector('.tooltip');
  const open = () => !!tip()?.matches(':popover-open');
  const close = document.getElementById('close');
  const copy = document.getElementById('copy');
  const menu = document.getElementById('menu');

  pointer('pointerover', close);
  await wait(100);
  check(!open(), 'The tooltip appeared before the hover delay');
  await wait(500);
  check(open() && tip().textContent === 'CloseEsc' && tip().querySelector('kbd')?.textContent === 'Esc',
    'Hovering did not show the label with its shortcut');
  check(tip().getAttribute('aria-hidden') === 'true', 'The tooltip duplicated the accessible name');

  pointer('pointerout', close, copy);
  pointer('pointerover', copy);
  check(open() && tip().textContent === 'Copy text', 'Moving to a neighbor did not reuse the warm tooltip');
  copy.dataset.tooltip = 'Copied';
  await wait(0);
  check(tip().textContent === 'Copied', 'The tooltip did not follow a label change');

  pointer('pointerdown', copy);
  check(!open(), 'Pressing the control left the tooltip open');
  pointer('pointerout', copy, document.body);
  await wait(700);

  pointer('pointerover', menu);
  await wait(600);
  check(!open(), 'An open menu still showed its tooltip');

  pointer('pointerover', close, null, 'touch');
  await wait(600);
  check(!open(), 'Touch showed a tooltip');
}

const assets = new URL('../assets/', import.meta.url);
const read = (path) => readFile(new URL(`.${path}`, assets));
const index = `<!doctype html><html><head><link rel="stylesheet" href="/style.css"></head><body>
  <button id="close" aria-label="Close" data-tooltip="Close" data-shortcut="Esc">x</button>
  <button id="copy" aria-label="Copy text" data-tooltip="Copy text">c</button>
  <button id="menu" aria-haspopup="menu" aria-expanded="true" data-tooltip="Menu">m</button>
  <script type="module">
    import '/tooltip.js';
    try {
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script></body></html>`;
const contentTypes = { '.js': 'text/javascript', '.css': 'text/css' };
const server = createServer(async (request, response) => {
  const { pathname } = new URL(request.url, 'http://localhost');
  try {
    response.setHeader('Content-Type', pathname === '/' ? 'text/html' : contentTypes[extname(pathname)] ?? 'application/octet-stream');
    response.end(pathname === '/' ? index : await read(pathname));
  } catch {
    response.writeHead(404).end();
  }
});
const directory = await mkdtemp(join(tmpdir(), 'flock-tooltip-dom-'));
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', `--user-data-dir=${directory}`, '--virtual-time-budget=5000', '--dump-dom',
    `http://127.0.0.1:${server.address().port}/`], { encoding: 'utf8', timeout: 25000 });
  const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the tooltip checks');
} finally {
  server.close();
  await rm(directory, { recursive: true, force: true });
}
