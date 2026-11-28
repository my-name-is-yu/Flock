import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

async function exercise(stageAttachments) {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const canvas = new OffscreenCanvas(3840, 2160);
  canvas.getContext('2d').fillRect(0, 0, 16, 16);
  const source = await canvas.convertToBlob({ type: 'image/webp' });
  check(source.type === 'image/webp', 'Test runtime should encode WebP');
  const picture = new File([source], 'Original.webp', { type: source.type });
  const report = new File([new Uint8Array([0, 65, 255])], 'Report.bin', { type: 'application/octet-stream' });
  const oversize = new File([new Uint8Array(10 * 1024 * 1024 + 1)], 'Large.webp', { type: source.type });
  const draft = { images: [] };
  await stageAttachments(draft, [oversize, picture, report], 8, () => true);
  check(draft.images.length === 2 && draft.error.startsWith('Large.webp:'), 'Oversized image must be identified');
  for (const [index, original] of [picture, report].entries()) {
    const { attachment, preview } = draft.images[index];
    const restored = Uint8Array.from(atob(attachment.dataBase64), (byte) => byte.charCodeAt(0));
    const bytes = new Uint8Array(await original.arrayBuffer());
    check(attachment.name === original.name && attachment.mimeType === original.type &&
      restored.length === bytes.length && restored.every((byte, i) => byte === bytes[i]),
    'Staged attachment must match the original file');
    check(Boolean(preview) === (original === picture), 'Only the WebP needs an image preview');
    if (preview) URL.revokeObjectURL(preview);
  }

  const stale = { images: [] };
  let current = true;
  const staging = stageAttachments(stale, [report], 8, () => current);
  current = false;
  await staging;
  check(stale.images.length === 0 && !stale.error, 'A file should be dropped when its draft changes');
}

const directory = mkdtempSync(join(tmpdir(), 'flock-screenshot-resize-'));
const assets = new URL('../assets/', import.meta.url);
const readAsset = (path) => readFileSync(new URL(`.${path}`, assets));
const module = readAsset('/composer-screenshots.mjs');
const html = `<!doctype html><script type="module">
  import { stageAttachments } from '/composer-screenshots.mjs';
  try {
    await (${exercise})(stageAttachments);
    await fetch('/result?passed');
  } catch (error) {
    await fetch('/result?error=' + encodeURIComponent(error.message));
  }
</script>`;
function appFixtures() {
  window.EventSource = undefined;
  const realFetch = window.fetch.bind(window);
  const bot = { id: 'release', name: 'Release', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  window.fetch = async (path, options) => {
    if (typeof path === 'string' && path.startsWith('/result?')) return realFetch(path, options);
    if (path === '/api/bots') return Response.json([bot]);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path === '/api/messages') {
      window.attachmentSubmission = JSON.parse(await options.body.text());
      return Response.json({ messageId: 1, status: 'received' });
    }
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: true });
    if (path.startsWith('/api/view?')) {
      return Response.json({
        bot, events: [], olderBefore: 0, presence: 'idle', activeSessions: [], activeTasks: [], parallelTasks: [],
        sessionTasks: [], sessionsBefore: 0, sessionCursor: 0, waits: [], canDecide: true,
        composerPerson: 'local-owner', limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 8 },
      });
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}
async function exerciseComposerDrop() {
  const byId = (id) => document.getElementById(id);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 60; attempt++) {
      if (predicate()) return;
      await new Promise((resolve) => setTimeout(resolve, 15));
    }
    check(predicate(), message);
  }
  await waitFor(() => byId('composer') && !byId('composer-container').hidden, 'Composer did not appear');
  check(byId('screenshot-input').accept === '', 'File input should accept general files');
  const canvas = new OffscreenCanvas(8, 8);
  canvas.getContext('2d').fillRect(0, 0, 8, 8);
  const blob = await canvas.convertToBlob({ type: 'image/webp' });
  const dataTransfer = new DataTransfer();
  dataTransfer.items.add(new File([blob], 'Dropped.webp', { type: 'image/webp' }));
  const composer = byId('composer');
  composer.dispatchEvent(new DragEvent('dragenter', { bubbles: true, cancelable: true, dataTransfer }));
  composer.dispatchEvent(new DragEvent('dragenter', { bubbles: true, cancelable: true, dataTransfer }));
  check(composer.classList.contains('dragover'), 'Drag enter did not highlight composer');
  composer.dispatchEvent(new DragEvent('dragleave', { bubbles: true, cancelable: true }));
  check(composer.classList.contains('dragover'), 'Leaving a child cleared dragover prematurely');
  composer.dispatchEvent(new DragEvent('dragleave', { bubbles: true, cancelable: true }));
  check(!composer.classList.contains('dragover'), 'Leaving composer did not clear dragover');
  composer.dispatchEvent(new DragEvent('dragover', { bubbles: true, cancelable: true, dataTransfer }));
  check(composer.classList.contains('dragover'), 'Dragover did not highlight composer');
  composer.dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, dataTransfer }));
  check(!composer.classList.contains('dragover'), 'Drop did not clear dragover');
  await waitFor(() => byId('screenshot-previews').childElementCount === 1, 'Dropped WebP image did not stage');
  check(byId('notice').textContent === 'Add a message to send files.' && byId('send').disabled,
    'Staging images without text did not show the guidance notice');
  byId('message').value = 'Please review';
  byId('message').dispatchEvent(new Event('input', { bubbles: true }));
  check(!byId('send').disabled && byId('notice').textContent === '',
    'Typing a message did not enable send and clear the image notice');
  const generalFile = new DataTransfer();
  generalFile.items.add(new File([new Uint8Array([0, 65, 255])], 'Report.bin', { type: 'application/octet-stream' }));
  const picker = byId('screenshot-input');
  picker.addEventListener('click', (event) => event.preventDefault());
  byId('attach-screenshots').click();
  picker.files = generalFile.files;
  picker.dispatchEvent(new Event('change', { bubbles: true }));
  await waitFor(() => byId('screenshot-previews').childElementCount === 2, 'General file did not stage');
  byId('send').click();
  await waitFor(() => window.attachmentSubmission, 'Composer did not send the staged attachments');
  const sent = window.attachmentSubmission;
  check(sent.text === 'Please review' && sent.attachments.length === 2 &&
    sent.attachments[0].name === 'Dropped.webp' && sent.attachments[0].mimeType === 'image/webp' &&
    sent.attachments[1].name === 'Report.bin' && sent.attachments[1].mimeType === 'application/octet-stream' &&
    sent.attachments[1].dataBase64 === 'AEH/', 'Message request changed the selected files');
}
const appHtml = readAsset('/index.html').toString().replace('<script type="module" src="/app.js"></script>', `
  <script>(${appFixtures})();</script>
  <script type="module">
    import '/app.js';
    try {
      await (${exerciseComposerDrop})();
      await fetch('/result?passed');
    } catch (error) {
      await fetch('/result?error=' + encodeURIComponent(error?.stack || error?.message || String(error)));
    }
  </script>`);
const contentTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
const server = createServer((request, response) => {
  const url = new URL(request.url, 'http://127.0.0.1');
  if (url.pathname === '/result') {
    response.end();
    server.emit('browserResult', url.searchParams.get('error'));
  } else if (url.pathname === '/composer-screenshots.mjs') {
    response.writeHead(200, { 'Content-Type': 'text/javascript' }).end(module);
  } else if (url.pathname === '/app') {
    response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }).end(appHtml);
  } else if (url.pathname === '/') {
    response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }).end(html);
  } else {
    try {
      const data = readAsset(url.pathname);
      response.writeHead(200, { 'Content-Type': contentTypes[url.pathname.slice(url.pathname.lastIndexOf('.'))] ?? 'application/octet-stream' }).end(data);
    } catch {
      response.writeHead(404).end();
    }
  }
});
let chrome;
const runCheck = async (path) => {
  const dataDir = mkdtempSync(join(tmpdir(), 'flock-screenshot-resize-'));
  try {
    chrome = spawn(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
      '--disable-background-networking', `--user-data-dir=${dataDir}`,
      `http://127.0.0.1:${server.address().port}${path}`], { stdio: 'ignore' });
    const result = once(server, 'browserResult', { signal: AbortSignal.timeout(20_000) });
    const exited = once(chrome, 'exit').then(([code]) => { throw new Error(`Chrome exited before the report (${code})`); });
    const [failure] = await Promise.race([result, exited]);
    if (failure) throw new Error(failure);
  } finally {
    if (chrome?.pid && chrome.exitCode === null && !chrome.signalCode) {
      const closed = once(chrome, 'exit');
      chrome.kill();
      await closed;
    }
    chrome = null;
    rmSync(dataDir, { recursive: true, force: true });
  }
};
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  await runCheck('/');
  await runCheck('/app?bot=release');
} finally {
  server.closeAllConnections();
  server.close();
  rmSync(directory, { recursive: true, force: true });
}
