import assert from 'node:assert/strict';
import { test } from 'node:test';
import { screenshotMessage } from '../assets/composer-screenshots.mjs';

const png = { name: 'demo.png', mimeType: 'image/png',
  dataBase64: 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC' };

test('retry keys follow the exact Bot, text, reply target, and ordered image payload', async () => {
  const fields = { botId: 'release', text: 'Could you look at this?', replyToMessageId: 7 };
  const attachments = [png, { ...png, name: 'other.png' }];
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
});
