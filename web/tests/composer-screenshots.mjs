import assert from 'node:assert/strict';
import { File } from 'node:buffer';
import { test } from 'node:test';
import { encodeScreenshot, screenshotMessage, stageScreenshots } from '../assets/composer-screenshots.mjs';

const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC', 'base64');
const pngFile = (name = 'demo.png', bytes = png) => new File([bytes], name, { type: 'image/png' });

test('encodes original image bytes and rejects unsupported or mismatched types', async () => {
  const name = '会議.png';
  assert.deepEqual(await encodeScreenshot(pngFile(name), []), {
    name, mimeType: 'image/png', dataBase64: png.toString('base64'),
  });
  const jpeg = Buffer.from([0xff, 0xd8, 0xff, 0xe0]);
  const detected = await encodeScreenshot(new File([jpeg], 'demo.jpg'), []);
  assert.equal(detected.mimeType, 'image/jpeg');
  assert.equal(detected.dataBase64, jpeg.toString('base64'));
  await assert.rejects(encodeScreenshot(new File([png], 'demo.svg', { type: 'image/svg+xml' }), []), /PNG or JPEG/);
  await assert.rejects(encodeScreenshot(pngFile('demo.png', jpeg), []), /valid PNG or JPEG/);
  const long = await encodeScreenshot(pngFile('画'.repeat(55)), []);
  assert.equal(Buffer.byteLength(long.name), 126);
});

test('enforces 4 files, 64 KiB per file, and the serialized attachment limit', async () => {
  const largePng = Buffer.alloc(65_536);
  png.copy(largePng);
  const large = await encodeScreenshot(pngFile('large.png', largePng), []);
  assert.equal(Buffer.from(large.dataBase64, 'base64').length, 65_536);
  await assert.rejects(encodeScreenshot(pngFile('huge.png', Buffer.concat([largePng, Buffer.from([0])])), []), /64 KiB/);
  const small = await encodeScreenshot(pngFile(), []);
  assert.ok(await encodeScreenshot(pngFile(), [small, small, small]));
  await assert.rejects(encodeScreenshot(pngFile(), [small, small, small, small]), /up to 4/);

  const overhead = Buffer.byteLength(JSON.stringify([large, { ...small, name: '', dataBase64: '' }]));
  const secondPng = Buffer.alloc(Math.floor((98_000 - overhead - 1) / 4) * 3);
  png.copy(secondPng);
  const second = await encodeScreenshot(pngFile('base.png', secondPng), []);
  const nameBytes = 98_000 - Buffer.byteLength(JSON.stringify([large, { ...second, name: '' }]));
  assert.ok(nameBytes > 0);
  const boundaryName = 'a'.repeat(nameBytes);
  const boundary = await encodeScreenshot(pngFile(boundaryName, secondPng), [large]);
  assert.equal(Buffer.byteLength(JSON.stringify([large, boundary])), 98_000);
  await assert.rejects(encodeScreenshot(pngFile(boundaryName + 'a', secondPng), [large]), /98,000-byte/);
});

test('retry keys follow the exact Bot, text, reply target, and ordered image payload', async () => {
  const fields = { botId: 'release', text: 'Could you look at this?', replyToMessageId: 7 };
  const attachments = [await encodeScreenshot(pngFile(), []), await encodeScreenshot(pngFile('other.png'), [])];
  const original = await screenshotMessage(fields, attachments);
  assert.deepEqual(await screenshotMessage({ ...fields }, attachments.map((item) => ({ ...item })), original.pending), original);
  assert.deepEqual(Object.keys(original.pending).sort(), ['clientMessageId', 'fingerprint']);

  for (const change of [{ botId: 'triage' }, { text: 'New text' }, { replyToMessageId: 8 }, { replyToMessageId: undefined }]) {
    const retry = await screenshotMessage({ ...fields, ...change }, attachments, original.pending);
    assert.notEqual(retry.payload.clientMessageId, original.payload.clientMessageId);
  }
  for (const change of [[attachments[0]], [...attachments].reverse(),
    [{ ...attachments[0], dataBase64: 'AQID' }, attachments[1]]]) {
    const retry = await screenshotMessage(fields, change, original.pending);
    assert.notEqual(retry.payload.clientMessageId, original.payload.clientMessageId);
  }
  const largePng = Buffer.alloc(65_536);
  png.copy(largePng);
  const large = await encodeScreenshot(pngFile('large.png', largePng), []);
  await assert.rejects(screenshotMessage({ ...fields, text: '\u0001'.repeat(9000) }, [large]), /too large together/);
});

test('drops an image whose file read finishes after the draft changes', async () => {
  const draft = { images: [] };
  let current = true;
  const file = pngFile();
  const staging = stageScreenshots(draft, [file], () => current);
  current = false;
  await staging;
  assert.deepEqual(draft, { images: [] });
});
