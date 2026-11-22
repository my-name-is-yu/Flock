import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { deflateSync } from 'node:zlib';

function pngChunk(type, data) {
  const chunk = Buffer.concat([Buffer.alloc(4), Buffer.from(type), data, Buffer.alloc(4)]);
  chunk.writeUInt32BE(data.length);
  let crc = 0xffffffff;
  for (const byte of chunk.subarray(4, -4)) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  chunk.writeUInt32BE((~crc) >>> 0, chunk.length - 4);
  return chunk;
}

function flatPng(width, height) {
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width);
  header.writeUInt32BE(height, 4);
  header[8] = 1; // One-bit grayscale keeps even 4K comfortably below 64 KiB.
  const rows = Buffer.alloc((Math.ceil(width / 8) + 1) * height);
  return Buffer.concat([Buffer.from('89504e470d0a1a0a', 'hex'), pngChunk('IHDR', header),
    pngChunk('IDAT', deflateSync(rows)), pngChunk('IEND', Buffer.alloc(0))]).toString('base64');
}

async function exercise(stageScreenshots, pngs) {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const png = (name) => new File([Uint8Array.from(atob(pngs[name]), (char) => char.charCodeAt(0))], name);
  const canvas = new OffscreenCanvas(8193, 1);
  canvas.getContext('2d').fillRect(0, 0, 8193, 1);
  const jpeg = await canvas.convertToBlob({ type: 'image/jpeg' });

  for (const file of [png('4k.png'), png('too-tall.png'), new File([jpeg], 'too-wide.jpg', { type: jpeg.type })]) {
    check(file.size <= 64 * 1024, `${file.name} must fit the file size limit`);
    const draft = { images: [] };
    await stageScreenshots(draft, [file], () => true);
    check(!draft.images.length && draft.error?.includes(`Crop or resize ${file.name}`),
      `Missing dimension error for ${file.name}`);
  }

  const draft = { images: [] };
  const boundary = png('fits.png');
  const tooLarge = png('over-pixel-limit.png');
  await stageScreenshots(draft, [tooLarge, boundary], () => true);
  check(draft.images.length === 1 && draft.images[0].attachment.name === boundary.name,
    'The exact edge and pixel limit should still stage after another image is rejected');
  check(draft.error?.includes(tooLarge.name), 'The rejected file should remain identified in the error');
  URL.revokeObjectURL(draft.images[0].preview);
}

const directory = mkdtempSync(join(tmpdir(), 'flock-screenshot-dimensions-'));
const module = readFileSync(new URL('../assets/composer-screenshots.mjs', import.meta.url));
const pngs = { '4k.png': flatPng(3840, 2160), 'too-tall.png': flatPng(1, 8193),
  'fits.png': flatPng(8192, 512), 'over-pixel-limit.png': flatPng(2048, 2049) };
const html = `<!doctype html><script type="module">
  import { stageScreenshots } from '/composer-screenshots.mjs';
  try {
    await (${exercise})(stageScreenshots, ${JSON.stringify(pngs)});
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
