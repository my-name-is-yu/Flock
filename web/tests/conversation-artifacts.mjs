import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { promisify } from 'node:util';

async function exercise(artifactPath) {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 120; attempt++) {
      if (predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error(message);
  }
  await waitFor(() => document.querySelector('#event-2 iframe') && document.querySelector('#event-3 .diagram-unavailable'),
    'The timeline did not show the saved diagram and the unavailable state');
  const conversation = document.getElementById('conversation');
  const reply = document.getElementById('event-2');
  const diagram = reply.querySelector('figure');
  const inline = diagram.querySelector('iframe');
  const bounds = diagram.getBoundingClientRect();
  const column = conversation.getBoundingClientRect();
  check(bounds.width > 0 && bounds.left >= column.left && bounds.right <= column.right &&
    inline.getBoundingClientRect().width <= bounds.width && conversation.scrollWidth <= conversation.clientWidth,
    'The diagram overflowed the conversation column');
  check(reply.querySelector('.event-text').textContent === 'Flock forwards the request to T3.' &&
    inline.getAttribute('src') === artifactPath && inline.title === 'Request and result' &&
    inline.getAttribute('sandbox') === '' && inline.inert,
    'The artifact was not isolated in the same reply as its explanation');
  const unavailable = document.getElementById('event-3');
  check(unavailable.querySelector('.event-text').textContent === 'The reply is still available.' &&
    unavailable.querySelector('[role=status]').textContent === 'Diagram unavailable.' && !unavailable.querySelector('iframe') &&
    unavailable.querySelector('.diagram-expand').hidden, 'A missing diagram hid the explanation or left an empty frame');
  check(document.querySelector('#event-4 a').getAttribute('href').startsWith('/api/artifacts/flock-'),
    'The external channel preview link changed');
  const expand = diagram.querySelector('button');
  const dialog = document.getElementById('detail');
  let expandedLoaded = false;
  dialog.addEventListener('load', () => { expandedLoaded = true; }, { capture: true, once: true });
  expand.focus();
  expand.click();
  await waitFor(() => dialog.querySelector('iframe'), 'The diagram dialog did not create a frame');
  await waitFor(() => expandedLoaded, 'The diagram could not be expanded');
  check(dialog.open && dialog.querySelector('h2').textContent === inline.title &&
    dialog.querySelector('iframe').getBoundingClientRect().width > inline.getBoundingClientRect().width,
    'The expanded diagram was not larger than the inline preview');
  document.getElementById('close-detail').click();
  check(!dialog.open && document.activeElement === expand && inline.isConnected,
    'Closing the diagram lost the inline preview or its keyboard focus');
  check(!window.artifactScriptRan, 'A script executed inside the diagram frame');
}

const assets = new URL('../assets/', import.meta.url);
const read = path => readFile(new URL(`.${path}`, assets));
const index = (await read('/index.html')).toString()
  .replace('<script type="module" src="/app.js"></script>', '<script type="module" src="/diagram-test.js"></script>');
const id = digit => `web-${digit.repeat(32)}`;
const path = `/api/messages/2/artifacts/${id('1')}`;
const bot = { id: 'release', name: 'Release' };
const time = Math.floor(Date.now() / 1000);
const message = (id, text, artifact) => ({ id, kind: artifact ? 'conversation_reply' : 'conversation_message', time: time + id,
  data: { text, ...(artifact && { artifacts: [artifact] }) } });
const events = [message(1, 'Diagram the flow from Flock to T3.'),
  message(2, 'Flock forwards the request to T3.', { id: id('1'), title: 'Request and result' }),
  message(3, 'The reply is still available.', { id: id('2'), title: 'Missing diagram' }),
  message(4, 'An external reply.', { id: `flock-${'3'.repeat(32)}`, title: 'External preview' })];
const view = { bot, events, olderBefore: 0, presence: 'idle', activeSessions: [], sessionTasks: [], waits: [], canDecide: true };
const mimeTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
const appPolicy = "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' blob:; frame-src 'self'";
const artifactPolicy = "sandbox; default-src 'none'; style-src 'unsafe-inline'; img-src data:; frame-ancestors 'self'";
const fixture = `window.EventSource = undefined;
addEventListener('message', (event) => {
  if (event.data === 'diagram-script-fixture') window.artifactScriptRan = true;
});
await import('/app.js');
try {
  await (${exercise})(${JSON.stringify(path)});
  parent.document.documentElement.dataset.flockResult = 'pass';
} catch (error) {
  parent.document.documentElement.dataset.flockResult = error.message;
}`;
let frameRequests = 0;
let imageRequests = 0;
const imageServer = createServer((_request, response) => { imageRequests++; response.writeHead(204).end(); });
const server = createServer(async (request, response) => {
  const { pathname, searchParams } = new URL(request.url, 'http://localhost');
  try {
    if (pathname === '/check') {
      const width = searchParams.has('mobile') ? 390 : 1260;
      response.writeHead(200, { 'Content-Type': 'text/html' }).end(
        `<!doctype html><iframe src="/?bot=release" width="${width}" height="820" style="border:0"></iframe>`);
    } else if (pathname === path) {
      response.writeHead(200, { 'Content-Type': 'text/html', 'Content-Security-Policy': artifactPolicy, 'Cache-Control': 'no-store' });
      if (request.method === 'GET') frameRequests++;
      response.end(`<!doctype html><style>svg{width:100%;height:auto}</style>
        <svg viewBox="0 0 300 80"><text y="35">Flock → T3 → Flock</text></svg>
        <img src="http://127.0.0.1:${imageServer.address().port}/image" alt="">
        <script>parent.postMessage('diagram-script-fixture', '*');</script>`);
    } else if (pathname.startsWith('/api/messages/')) {
      response.writeHead(404).end();
    } else if (pathname.startsWith('/api/')) {
      response.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify(pathname === '/api/bots' ? [bot] : view));
    } else if (pathname === '/') {
      response.writeHead(200, { 'Content-Type': 'text/html', 'Content-Security-Policy': appPolicy }).end(index);
    } else {
      const body = pathname === '/diagram-test.js' ? fixture : await read(pathname);
      response.writeHead(200, { 'Content-Type': mimeTypes[extname(pathname)] }).end(body);
    }
  } catch {
    response.writeHead(404).end();
  }
});
const directory = await mkdtemp(join(tmpdir(), 'flock-artifacts-dom-'));
try {
  server.listen(0, '127.0.0.1');
  imageServer.listen(0, '127.0.0.1');
  await Promise.all([once(server, 'listening'), once(imageServer, 'listening')]);
  for (const width of ['desktop', 'mobile']) {
    frameRequests = 0;
    const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
      '--disable-background-networking', '--run-all-compositor-stages-before-draw', `--user-data-dir=${directory}`,
      '--window-size=1300,930', '--virtual-time-budget=15000',
      '--dump-dom', `http://127.0.0.1:${server.address().port}/check?${width}`], { encoding: 'utf8', timeout: 15000 });
    const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
    if (result?.[1] !== 'pass') throw new Error(`${width}: ${result?.[1] ?? 'The browser did not finish the diagram checks'}`);
    if (frameRequests < 2) throw new Error(`${width}: the browser did not load the inline and expanded documents`);
    if (imageRequests) throw new Error(`${width}: the browser requested a diagram's external image`);
  }
} finally {
  server.close();
  imageServer.close();
  await rm(directory, { recursive: true, force: true });
}
