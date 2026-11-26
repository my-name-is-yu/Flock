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
  window.pauseChanges = [];
  window.sentMessages = [];
  window.sharePausedBot = () => {
    Object.assign(bot, { canDelete: false, paused: true });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.unconfigureBot = (canDelete) => {
    Object.assign(bot, { canDelete, paused: false, model: '' });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.fetch = async (path, { body } = {}) => {
    if (path === '/api/bots') return Response.json([bot]);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: bot.canDelete });
    if (path.startsWith('/api/view?')) return Response.json({
      bot, events: [], olderBefore: 0, presence: 'idle', activeSessions: [], sessionTasks: [], sessionsBefore: 0,
      sessionCursor: 0, waits: [], canDecide: true, composerPerson: 'local-owner',
      limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
    });
    if (path === '/api/bots/pause') {
      const change = JSON.parse(body);
      pauseChanges.push(change);
      bot.paused = change.paused;
      return Response.json({ paused: bot.paused });
    }
    if (path === '/api/messages') {
      sentMessages.push(JSON.parse(body));
      return Response.json({ messageId: 1 });
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exercise() {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 50; attempt++) {
      if (predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    check(predicate(), message);
  }
  const settings = $('#detail');
  const confirmation = $('#bot-pause');
  const pauseSetting = () => $('#bot-pause-setting');
  const pauseBanner = $('#composer-banner');
  const resume = $('#composer-banner-action');
  const composer = $('#composer');
  const message = $('#message');
  const menuItems = () => [...document.querySelectorAll('#bot-menu [role="menuitem"]')];

  await waitFor(() => !composer.hidden && !$('#composer-container').hidden, 'The active Bot had no composer');
  const menu = $('.row-menu');
  message.value = 'Draft for after resume';
  message.dispatchEvent(new Event('input'));
  $('#profile-avatar-button').click();
  check(settings.open && pauseSetting().textContent === 'Pause', 'The owner could not find Pause in settings');
  pauseSetting().click();
  check(confirmation.open && $('#bot-pause-title').textContent === 'Pause Release?', 'Pause did not confirm the Bot');
  $('#bot-pause-cancel').click();
  check(!confirmation.open && !pauseChanges.length, 'Cancelling changed the Bot’s state');
  await new Promise(resolve => setTimeout(resolve, 20));
  pauseSetting().click();
  $('#bot-pause-confirm').click();
  await waitFor(() => !confirmation.open && pauseSetting().textContent === 'Resume', 'Settings did not switch to Resume');
  check(pauseChanges.length === 1 && pauseChanges[0].botId === 'release' && pauseChanges[0].paused === true,
    'Settings did not pause the selected Bot');
  $('#close-detail').click();
  check($('#profile-status').textContent === 'Paused' && $('.bot-preview').textContent === 'Paused',
    'The conversation header or sidebar did not show Paused');
  check(composer.hidden && message.readOnly && !pauseBanner.hidden && !resume.hidden,
    'The paused Bot still accepted text or hid the owner’s Resume action');
  composer.dispatchEvent(new Event('submit', { cancelable: true }));
  check(!sentMessages.length, 'Submitting the hidden form sent a message');
  resume.click();
  await waitFor(() => !composer.hidden && pauseBanner.hidden, 'The composer did not return after Resume');
  check(pauseChanges.length === 2 && pauseChanges[1].paused === false && message.value === 'Draft for after resume',
    'Resume did not preserve the local draft');

  menu.click();
  menuItems().find(item => item.textContent === 'Pause…').click();
  $('#bot-pause-confirm').click();
  await waitFor(() => !confirmation.open && !pauseBanner.hidden, 'The Bot menu could not pause');
  menu.click();
  menuItems().find(item => item.textContent === 'Resume').click();
  $('#bot-pause-confirm').click();
  await waitFor(() => !confirmation.open && pauseBanner.hidden, 'The Bot menu could not resume');

  window.sharePausedBot();
  await waitFor(() => !pauseBanner.hidden, 'A paused Bot still showed its composer to a member');
  check(resume.hidden, 'A member could resume from the composer');
  menu.click();
  check(menuItems().every(item => !['Pause…', 'Resume'].includes(item.textContent)), 'A member could manage pause from the menu');
  menu.click();
  $('#profile-avatar-button').click();
  check(settings.open && !pauseSetting(), 'A member could manage pause from settings');
  $('#close-detail').click();

  window.unconfigureBot(false);
  await waitFor(() => pauseBanner.dataset.state === 'setup', 'A Bot without a model still showed its composer');
  check(composer.hidden && resume.hidden && $('#composer-banner-title').textContent === 'Release isn’t set up yet',
    'A member could send to, or set up, a Bot without a model');
  window.unconfigureBot(true);
  await waitFor(() => !resume.hidden, 'The owner could not set up a Bot without a model');
  check(resume.textContent === 'Choose a model', 'The setup action did not say what it does');
  resume.click();
  check(settings.open && !$('.settings-workspace-form').hidden, 'Choose a model did not open the model editor');
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
const directory = await mkdtemp(join(tmpdir(), 'flock-bot-pause-dom-'));
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { stdout } = await promisify(execFile)(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', `--user-data-dir=${directory}`, '--virtual-time-budget=3000', '--dump-dom',
    `http://127.0.0.1:${server.address().port}/?bot=release`], { encoding: 'utf8', timeout: 25000 });
  const result = stdout.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the pause checks');
} finally {
  server.close();
  await rm(directory, { recursive: true, force: true });
}
