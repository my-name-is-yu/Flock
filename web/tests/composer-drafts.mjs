import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createComposerDraftStore } from '../assets/composer-drafts.mjs';

class SessionStorage extends Map {
  getItem(key) { return this.get(key) ?? null; }
  setItem(key, value) { this.set(key, value); }
  removeItem(key) { this.delete(key); }
}

const question = { kind: 'wait', messageId: 17, summary: 'How many days should the report cover?' };
const result = { kind: 'result', messageId: 31, summary: 'The report is ready.' };

test('restores ordinary and anchored drafts after dismissal, switching targets, and reload', () => {
  const storage = new SessionStorage();
  const load = () => createComposerDraftStore(() => storage)('ops', null);
  let composer = load();
  composer.edit('次回の集計について相談したい。');
  composer.openReply(question);
  composer.edit('直近7日分でお願いします。');
  composer.closeReply();
  assert.equal(composer.text, '次回の集計について相談したい。');
  composer.openReply(result, 'Share the results');
  composer.openReply(question, '14 days');
  assert.equal(composer.text, '直近7日分でお願いします。');
  composer = load();
  assert.deepEqual(composer.reply, { kind: 'wait', messageId: 17 });
  assert.equal(composer.text, '直近7日分でお願いします。');
  composer.openReply(result);
  assert.equal(composer.text, 'Share the results');
  composer.closeReply();
  composer = load();
  assert.equal(composer.reply, null);
  assert.equal(composer.text, '次回の集計について相談したい。');
  composer.edit('');
  assert.equal(load().text, '');
});

test('replaces untouched suggestions and keeps edited replies', () => {
  const storage = new SessionStorage();
  const load = () => createComposerDraftStore(() => storage)('ops', null);
  const message = { kind: 'message', messageId: 28, summary: 'Choose a branch.' };
  let composer = load();
  composer.openReply(message, 'main');
  composer.openReply(message, 'release');
  assert.equal(composer.text, 'release');
  composer = load();
  composer.openReply(message, 'docs');
  assert.equal(composer.text, 'docs');
  composer.edit('Use the docs branch once the tests pass.');
  composer = load();
  composer.openReply(message, 'main');
  assert.equal(composer.text, 'Use the docs branch once the tests pass.');
});

test('separates browser sessions, authenticated people and Bots', () => {
  const storage = new SessionStorage();
  const load = () => createComposerDraftStore(() => storage);
  load()('ops', 'alice').edit('Alice draft');
  const drafts = load();
  assert.equal(drafts('ops', 'alice').text, 'Alice draft');
  assert.equal(drafts('ops', 'bob').text, '');
  assert.equal(drafts('review', 'alice').text, '');
  assert.equal(drafts('ops', null).text, '');
  assert.equal(createComposerDraftStore(() => new SessionStorage())('ops', 'alice').text, '');
});

test('keeps pending IDs per target and clears only the draft whose delivery succeeds', () => {
  const storage = new SessionStorage();
  const load = () => createComposerDraftStore(() => storage)('ops', 'alice');
  let composer = load();
  composer.edit('Update from Ops');
  const original = composer.prepareSend();
  composer.openReply(question);
  composer.edit('Update from Ops');
  const reply = composer.prepareSend();
  assert.equal(reply.replyToMessageId, question.messageId);
  assert.notEqual(reply.clientMessageId, original.clientMessageId);

  composer = load();
  assert.deepEqual(composer.prepareSend(), reply);
  composer.settleSend(reply, true);
  assert.equal(composer.reply, null);
  assert.deepEqual(composer.prepareSend(), original);
  composer.settleSend(original, false);
  assert.equal(composer.text, original.text);
  const retried = composer.prepareSend();
  assert.notEqual(retried.clientMessageId, original.clientMessageId);
  composer.settleSend(retried, true);
  assert.equal(load().text, '');
  assert.equal(load().textAt(question.messageId), '');
  assert.equal(storage.size, 0);
});

test('recovers legacy uncertain sends for the local owner and ignores them for shared identities', () => {
  const payload = { botId: 'ops', replyToMessageId: question.messageId, text: '7 days', clientMessageId: 'original-key' };
  const key = 'flock-pending-message:ops';
  const storage = new SessionStorage([[key, JSON.stringify({ payload, reply: question })]]);
  const drafts = createComposerDraftStore(() => storage);
  assert.equal(drafts('ops', 'alice').text, '');
  assert.equal(drafts('ops', undefined).text, '');
  assert.equal(drafts('ops', null).text, payload.text);
  assert.equal(storage.has(key), false);
  assert.deepEqual(createComposerDraftStore(() => storage)('ops', null).prepareSend(), payload);
});

test('keeps drafts and exact retries usable while the browser blocks storage', () => {
  const drafts = createComposerDraftStore(() => { throw new Error('Storage blocked'); });
  const composer = drafts('ops', null);
  composer.edit('Local draft');
  const payload = composer.prepareSend();
  assert.equal(drafts('ops', null).text, 'Local draft');
  assert.deepEqual(composer.prepareSend(), payload);
});
