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
  const expand = diagram.querySelector('.diagram-expand');
  const content = (frame, id) => frame.contentDocument?.getElementById(id);
  await waitFor(() => content(inline, 'diagram-end') && !expand.disabled, 'The inline diagram did not load');
  const bounds = diagram.getBoundingClientRect();
  const column = conversation.getBoundingClientRect();
  check(bounds.width > 0 && bounds.left >= column.left && bounds.right <= column.right &&
    inline.getBoundingClientRect().width <= bounds.width && conversation.scrollWidth <= conversation.clientWidth,
    'The diagram overflowed the conversation column');
  check(bounds.height > 0 && bounds.height < 550 && content(inline, 'diagram').offsetHeight > 1500,
    'A tall diagram was not kept within a bounded inline preview');
  check(reply.querySelector('.event-text').textContent === 'Flock forwards the request to T3.' &&
    inline.getAttribute('src') === artifactPath && inline.title === 'Request and result' &&
    inline.getAttribute('sandbox') === 'allow-same-origin' && inline.inert,
    'The artifact was not isolated in the same reply as its explanation');
  const compact = () => document.querySelector('#event-5 .diagram-frame');
  const hasCompactHeight = (height) => compact() && content(compact(), 'compact') &&
    Math.abs(compact().getBoundingClientRect().height - height) < 2;
  const desktop = innerWidth > 700;
  await waitFor(() => hasCompactHeight(desktop ? 200 : 252), 'A short diagram did not follow its responsive content height');
  const theme = (mode) => document.querySelector(`[data-theme-mode="${mode}"]`).click();
  const diagramColor = (frame) => frame.contentWindow.getComputedStyle(content(frame, 'diagram')).backgroundColor;
  theme('light');
  await waitFor(() => diagramColor(inline) === 'rgb(230, 240, 250)', 'The diagram did not use Flock’s light theme');
  theme('dark');
  await waitFor(() => diagramColor(inline) === 'rgb(21, 32, 43)', 'The diagram did not use Flock’s dark theme');
  const unavailable = document.getElementById('event-3');
  check(unavailable.querySelector('.event-text').textContent === 'The reply is still available.' &&
    unavailable.querySelector('[role=status]').textContent === 'Diagram unavailable.' && !unavailable.querySelector('iframe') &&
    unavailable.querySelector('.diagram-expand').hidden, 'A missing diagram hid the explanation or left an empty frame');
  check(document.querySelector('#event-4 a').getAttribute('href').startsWith('/api/artifacts/flock-'),
    'The external channel preview link changed');
  const dialog = document.getElementById('detail');
  expand.focus();
  expand.click();
  const viewport = dialog.querySelector('.diagram-preview');
  const frame = () => viewport?.querySelector('iframe');
  await waitFor(() => frame() && content(frame(), 'diagram-end') && viewport.scrollHeight > viewport.clientHeight + 800,
    'The expanded viewport did not size itself to a tall diagram');
  check(dialog.open && dialog.querySelector('h2').textContent === inline.title && frame().inert &&
    frame().getAttribute('sandbox') === 'allow-same-origin' && viewport.tabIndex === 0,
    'The diagram dialog did not provide a focusable scroller around a static frame');
  await waitFor(() => diagramColor(frame()) === 'rgb(21, 32, 43)', 'The expanded diagram did not use Flock’s theme');
  viewport.focus();
  check(document.activeElement === viewport, 'Keyboard focus could not enter the diagram scroller');
  viewport.scrollTop = viewport.scrollHeight;
  const scale = () => frame().getBoundingClientRect().width / frame().offsetWidth;
  const lastLabel = content(frame(), 'diagram-end').getBoundingClientRect();
  const labelTop = frame().getBoundingClientRect().top + lastLabel.top * scale();
  const labelBottom = labelTop + lastLabel.height * scale();
  const scrollBounds = viewport.getBoundingClientRect();
  check(viewport.scrollTop > 800 && labelTop >= scrollBounds.top && labelBottom <= scrollBounds.bottom + 2,
    'The final step was clipped at the end of the expanded diagram');

  const control = (label) => dialog.querySelector(`button[aria-label="${label}"]`);
  const initialScale = scale();
  control('Zoom in').click();
  await waitFor(() => scale() > initialScale + .15, 'Zoom in did not magnify the diagram');
  control('Reset zoom').click();
  await waitFor(() => Math.abs(scale() - initialScale) < .02, 'Reset did not restore the diagram scale');
  control('Zoom out').click();
  await waitFor(() => scale() < initialScale - .15, 'Zoom out did not reduce the diagram scale');
  if (desktop) {
    const width = dialog.getBoundingClientRect().width;
    control('Full screen').click();
    await waitFor(() => control('Exit full screen') && dialog.getBoundingClientRect().width > width,
      'Full screen did not enlarge the diagram viewport');
    control('Exit full screen').click();
    await waitFor(() => Math.abs(dialog.getBoundingClientRect().width - width) < 2,
      'Exiting full screen did not restore the diagram dialog');
  } else {
    check(Math.abs(dialog.offsetWidth - innerWidth) < 2 && Math.abs(dialog.offsetHeight - innerHeight) < 2 &&
      control('Full screen').getClientRects().length === 0,
      'The mobile diagram did not open across the full viewport');
  }
  document.getElementById('close-detail').click();
  check(!dialog.open && document.activeElement === expand && inline.isConnected,
    'Closing the diagram lost the inline preview or its keyboard focus');
  check(!window.artifactScriptRan, 'A script executed inside the diagram frame');
  check(!window.artifactEventRan, 'An inline event handler executed inside the diagram frame');
}

const assets = new URL('../assets/', import.meta.url);
const read = path => readFile(new URL(`.${path}`, assets));
const index = (await read('/index.html')).toString()
  .replace('<script type="module" src="/app.js"></script>', '<script type="module" src="/diagram-test.js"></script>');
const id = digit => `web-${digit.repeat(32)}`;
const path = `/api/messages/2/artifacts/${id('1')}`;
const compactPath = `/api/messages/5/artifacts/${id('5')}`;
const bot = { id: 'release', name: 'Release' };
const time = Math.floor(Date.now() / 1000);
const message = (id, text, artifact) => ({ id, kind: artifact ? 'conversation_reply' : 'conversation_message', time: time + id,
  data: { text, ...(artifact && { artifacts: [artifact] }) } });
const events = [message(1, 'Diagram the flow from Flock to T3.'),
  message(2, 'Flock forwards the request to T3.', { id: id('1'), title: 'Request and result' }),
  message(3, 'The reply is still available.', { id: id('2'), title: 'Missing diagram' }),
  message(4, 'An external reply.', { id: `flock-${'3'.repeat(32)}`, title: 'External preview' }),
  message(5, 'A compact diagram.', { id: id('5'), title: 'Compact flow' })];
const view = { bot, events, olderBefore: 0, presence: 'idle', activeSessions: [], sessionTasks: [], waits: [], canDecide: true };
const mimeTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
const appPolicy = "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' blob:; frame-src 'self'";
const artifactPolicy = "sandbox allow-same-origin; default-src 'none'; script-src 'none'; style-src 'unsafe-inline'; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'self'";
const fixture = `window.EventSource = undefined;
addEventListener('message', (event) => {
  if (event.data === 'diagram-script-fixture') window.artifactScriptRan = true;
  if (event.data === 'diagram-event-fixture') window.artifactEventRan = true;
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
      response.end(`<!doctype html><style>
        body{margin:0}
        #diagram{background:rgb(230,240,250);min-height:1750px;display:flex;flex-direction:column;justify-content:space-between}
        @media(prefers-color-scheme:dark){#diagram{background:rgb(21,32,43)}}
        svg{width:100%;height:auto}
        </style><main id="diagram">
        <svg viewBox="0 0 300 80"><text y="35">Flock → T3 → Flock</text></svg>
        <p id="diagram-end">Final step</p></main>
        <img src="http://127.0.0.1:${imageServer.address().port}/image" alt="">
        <img src="/diagram-test-image" alt="">
        <img src="data:image/png;base64,bm90X2FfcG5n" alt="" onerror="parent.postMessage('diagram-event-fixture', '*')">
        <script>parent.postMessage('diagram-script-fixture', '*');</script>`);
    } else if (pathname === compactPath) {
      response.writeHead(200, { 'Content-Type': 'text/html', 'Content-Security-Policy': artifactPolicy, 'Cache-Control': 'no-store' });
      response.end(`<!doctype html><style>body{margin:0} #compact{height:200px}
        @media(max-width:480px){#compact{height:252px}}</style><div id="compact">Compact flow</div>`);
    } else if (pathname === '/diagram-test-image') {
      imageRequests++;
      response.writeHead(204).end();
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
