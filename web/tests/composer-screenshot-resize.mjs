import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

async function exercise(resizeScreenshot, stageScreenshots) {
  const check = (value, message) => { if (!value) throw new Error(message); };
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
    return new File([blob], type === 'image/png' ? 'Screen.png' : 'Screen.jpg', { type });
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

  const wide = await picture(3840, 2160, 'image/png');
  const resized = await resizeScreenshot(wide);
  check(await dimensions(resized) === '2048x1152', 'A 4K PNG should scale down and retain its aspect ratio');

  const tall = await picture(1800, 3200, 'image/jpeg');
  check(await dimensions(await resizeScreenshot(tall)) === '1152x2048', 'A tall JPEG should scale down');

  const dense = await picture(2048, 2048, 'image/png', true);
  check(dense.size > 2 * 1024 * 1024 && dense.size < 20 * 1024 * 1024, 'Dense source must exercise the byte limit');
  const prepared = await resizeScreenshot(dense);
  check(prepared.type === 'image/jpeg' && prepared.size <= 2 * 1024 * 1024, 'Dense PNG should fit as a JPEG');
  check((await dimensions(prepared)).split('x').every((edge) => Number(edge) <= 2048), 'Dense output dimensions should fit');

  const mismatch = new File([smallJpeg], 'mismatch.png', { type: 'image/png' });
  const corrupt = new File([await small.slice(0, 16).arrayBuffer()], 'corrupt.png', { type: 'image/png' });
  for (const file of [mismatch, corrupt]) {
    const draft = { images: [] };
    await stageScreenshots(draft, [file], () => true);
    check(draft.images.length === 0 && draft.error, 'Invalid image should not be staged');
  }

  const draft = { images: [] };
  const unnamedType = new File([small], '画'.repeat(55) + '.png');
  await stageScreenshots(draft, [dense, wide, tall, unnamedType, small], () => true);
  check(draft.images.length === 4 && draft.error === 'Attach up to 4 screenshots per message.', 'Composer should keep four images');
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
  const staging = stageScreenshots(stale, [small], () => current);
  current = false;
  await staging;
  check(stale.images.length === 0 && !stale.error, 'An image should be dropped when its draft changes');
}

const directory = mkdtempSync(join(tmpdir(), 'flock-screenshot-resize-'));
const module = readFileSync(new URL('../assets/composer-screenshots.mjs', import.meta.url));
const html = `<!doctype html><script type="module">
  import { resizeScreenshot, stageScreenshots } from '/composer-screenshots.mjs';
  try {
    await (${exercise})(resizeScreenshot, stageScreenshots);
    await fetch('/result?passed');
  } catch (error) {
    await fetch('/result?error=' + encodeURIComponent(error.message));
  }
</script>`;
const server = createServer((request, response) => {
  const url = new URL(request.url, 'http://127.0.0.1');
  if (url.pathname === '/result') {
    response.end();
    server.emit('browserResult', url.searchParams.get('error'));
  } else if (url.pathname === '/composer-screenshots.mjs') {
    response.writeHead(200, { 'Content-Type': 'text/javascript' }).end(module);
  } else {
    response.writeHead(200, { 'Content-Type': 'text/html' }).end(html);
  }
});
let chrome;
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  chrome = spawn(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', `--user-data-dir=${directory}`,
    `http://127.0.0.1:${server.address().port}`], { stdio: 'ignore' });
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
  server.closeAllConnections();
  server.close();
  rmSync(directory, { recursive: true, force: true });
}
