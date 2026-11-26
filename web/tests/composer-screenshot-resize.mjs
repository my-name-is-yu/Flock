import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

async function exercise(resizeScreenshot, stageScreenshots) {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const attachmentLimit = 4;
  async function picture(width, height, type, noisy = false) {
    const canvas = new OffscreenCanvas(width, height);
    const context = canvas.getContext('2d', { alpha: false });
    if (noisy) {
      const image = context.createImageData(width, height);
      let seed = 17;
      for (let offset = 0; offset < image.data.length; offset += 4) {
        seed ^= seed << 13;
        seed ^= seed >>> 17;
        seed ^= seed << 5;
        image.data[offset] = seed & 255;
        image.data[offset + 1] = (seed >>> 8) & 255;
        image.data[offset + 2] = (seed >>> 16) & 255;
        image.data[offset + 3] = 255;
      }
      context.putImageData(image, 0, 0);
    }
    const blob = await canvas.convertToBlob({ type });
    const extension = type === 'image/png' ? 'png' : type === 'image/webp' ? 'webp' : 'jpg';
    return new File([blob], `Screen.${extension}`, { type });
  }
  async function dimensions(blob) {
    const bitmap = await createImageBitmap(blob);
    const { width, height } = bitmap;
    bitmap.close();
    return [width, height].join('x');
  }

  const small = await picture(48, 24, 'image/png');
  check(await resizeScreenshot(small) === small, 'A fitting PNG should keep its original bytes');
  const smallJpeg = await picture(48, 24, 'image/jpeg');
  check(await resizeScreenshot(smallJpeg) === smallJpeg, 'A fitting JPEG should keep its original bytes');
  const smallWebp = await picture(48, 24, 'image/webp');
  check(smallWebp.type === 'image/webp', 'Test runtime must encode a WebP blob');
  const transcodedWebp = await resizeScreenshot(smallWebp);
  check(transcodedWebp.type === 'image/png' || transcodedWebp.type === 'image/jpeg',
    'A WebP image should transcode to PNG or JPEG');
  const transparentWebp = new OffscreenCanvas(48, 24);
  transparentWebp.getContext('2d').fillRect(0, 0, 12, 12);
  const transparentWebpFile = new File([await transparentWebp.convertToBlob({ type: 'image/webp' })], 'Clear.webp', { type: 'image/webp' });
  const transparentWebpResult = await resizeScreenshot(transparentWebpFile);
  check(transparentWebpResult.type === 'image/png', 'A transparent WebP should transcode to PNG');
  const webpDraft = { images: [] };
  await stageScreenshots(webpDraft, [smallWebp], attachmentLimit, () => true);
  check(webpDraft.images.length === 1 && /\.(png|jpg)$/.test(webpDraft.images[0].attachment.name) &&
    webpDraft.images[0].attachment.mimeType === transcodedWebp.type,
    'Staged WebP should use a transcoded PNG or JPEG attachment');
  for (const image of webpDraft.images) URL.revokeObjectURL(image.preview);

  const wide = await picture(3840, 2160, 'image/png');
  const resized = await resizeScreenshot(wide);
  check(await dimensions(resized) === '2048x1152', 'A 4K PNG should scale down and retain its aspect ratio');

  const tall = await picture(1800, 3200, 'image/jpeg');
  check(await dimensions(await resizeScreenshot(tall)) === '1152x2048', 'A tall JPEG should scale down');

  const transparent = new OffscreenCanvas(3840, 2160);
  const transparentContext = transparent.getContext('2d');
  transparentContext.fillStyle = 'white';
  transparentContext.fillRect(0, 0, 100, 100);
  const transparentFile = new File([await transparent.convertToBlob()], 'Transparent.png', { type: 'image/png' });
  const transparentResult = await resizeScreenshot(transparentFile);
  const transparentBitmap = await createImageBitmap(transparentResult);
  const sample = new OffscreenCanvas(100, 100).getContext('2d');
  sample.drawImage(transparentBitmap, 0, 0);
  transparentBitmap.close();
  check(transparentResult.type === 'image/png' && sample.getImageData(0, 0, 1, 1).data[3] === 255 &&
    sample.getImageData(90, 90, 1, 1).data[3] === 0, 'Resizing must preserve opaque and transparent pixels');

  const dense = await picture(2048, 2048, 'image/png', true);
  check(dense.size > 2 * 1024 * 1024 && dense.size < 20 * 1024 * 1024, 'Dense source must exercise the byte limit');
  const prepared = await resizeScreenshot(dense);
  check(prepared.type === 'image/jpeg' && prepared.size <= 2 * 1024 * 1024, 'Dense PNG should fit as a JPEG');
  check((await dimensions(prepared)).split('x').every((edge) => Number(edge) <= 2048), 'Dense output dimensions should fit');

  const mismatch = new File([smallJpeg], 'mismatch.png', { type: 'image/png' });
  const corrupt = new File([await small.slice(0, 16).arrayBuffer()], 'corrupt.png', { type: 'image/png' });
  for (const file of [mismatch, corrupt]) {
    const draft = { images: [] };
    await stageScreenshots(draft, [file], attachmentLimit, () => true);
    check(draft.images.length === 0 && draft.error, 'Invalid image should not be staged');
  }

  const draft = { images: [] };
  const unnamedType = new File([small], '画'.repeat(55) + '.png');
  await stageScreenshots(draft, [dense, wide, tall, unnamedType, small], attachmentLimit, () => true);
  check(draft.images.length === 4 && draft.error === 'Attach up to 4 images per message.', 'Composer should keep four images');
  const { attachment, preview } = draft.images[0];
  const previewBlob = await fetch(preview).then((response) => response.blob());
  check(attachment.name === 'Screen.jpg' && attachment.mimeType === previewBlob.type &&
    atob(attachment.dataBase64).length === previewBlob.size && previewBlob.size <= 2 * 1024 * 1024,
  'The attachment and preview should use the prepared JPEG');
  check(new TextEncoder().encode(draft.images[3].attachment.name).length === 126,
    'An unknown MIME should be sniffed and a long filename should be truncated by UTF-8 length');
  for (const image of draft.images) URL.revokeObjectURL(image.preview);

  const stale = { images: [] };
  let current = true;
  const staging = stageScreenshots(stale, [small], attachmentLimit, () => current);
  current = false;
  await staging;
  check(stale.images.length === 0 && !stale.error, 'An image should be dropped when its draft changes');
}

const directory = mkdtempSync(join(tmpdir(), 'flock-screenshot-resize-'));
const assets = new URL('../assets/', import.meta.url);
const readAsset = (path) => readFileSync(new URL(`.${path}`, assets));
const module = readAsset('/composer-screenshots.mjs');
const html = `<!doctype html><script type="module">
  import { resizeScreenshot, stageScreenshots } from '/composer-screenshots.mjs';
  try {
    await (${exercise})(resizeScreenshot, stageScreenshots);
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
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: true });
    if (path.startsWith('/api/view?')) {
      return Response.json({
        bot, events: [], olderBefore: 0, presence: 'idle', activeSessions: [], activeTasks: [], parallelTasks: [],
        sessionTasks: [], sessionsBefore: 0, sessionCursor: 0, waits: [], canDecide: true,
        composerPerson: 'local-owner', limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
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
  check(byId('screenshot-input').accept === 'image/png,image/jpeg,image/webp', 'File input must accept WebP images');
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
  check(byId('notice').textContent === 'Add a message to send images.' && byId('send').disabled,
    'Staging images without text did not show the guidance notice');
  byId('message').value = 'Please review';
  byId('message').dispatchEvent(new Event('input', { bubbles: true }));
  check(!byId('send').disabled && byId('notice').textContent === '',
    'Typing a message did not enable send and clear the image notice');
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
    response.writeHead(200, { 'Content-Type': 'text/html' }).end(appHtml);
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
