import assert from 'node:assert/strict';
import { refreshEventHistory } from '../assets/event-history.mjs';

const page = (ids, olderBefore) => ({ events: ids.map(id => ({ id })), olderBefore });
const emptyHistory = () => ({ events: new Map(), olderBefore: 0 });
const ids = history => [...history.events.keys()].sort((a, b) => a - b);
const noPage = () => assert.fail('unexpected history request');
const current = () => true;
const history = emptyHistory();
await refreshEventHistory(history, page([3, 4], 3), noPage, current);
await refreshEventHistory(history, page([4, 5], 4), noPage, current);
assert.deepEqual(ids(history), [3, 4, 5]);
assert.equal(history.olderBefore, 3);

const latest = page([9, 10], 9);
const older = new Map([[9, page([7, 8], 7)], [7, page([4, 5], 4)]]);
let invalidReads = 0;
await assert.rejects(refreshEventHistory(history, latest, () => {
  if (++invalidReads > 1) throw Error('repeated read');
  return latest;
}, current));
assert.equal(invalidReads, 1);
await assert.rejects(refreshEventHistory(history, latest, before => {
  if (before === 7) throw Error('interrupted');
  return older.get(before);
}, current));
let selected = true;
await refreshEventHistory(history, latest, before => {
  selected = false;
  return older.get(before);
}, () => selected);
assert.deepEqual(ids(history), [3, 4, 5]);
assert.equal(history.newestEvent, 5);
assert.equal(history.olderBefore, 3);

await refreshEventHistory(history, latest, before => {
  // An earlier-history request finishes while reconnect is reading the missing pages.
  if (before === 7) {
    history.events.set(1, { id: 1 });
    history.olderBefore = 0;
  }
  return older.get(before);
}, current);
assert.deepEqual(ids(history), [1, 3, 4, 5, 7, 8, 9, 10]);
assert.equal(history.newestEvent, 10);
assert.equal(history.olderBefore, 0);

const previouslyEmpty = emptyHistory();
await refreshEventHistory(previouslyEmpty, page([], 0), noPage, current);
await refreshEventHistory(previouslyEmpty, page([3, 4], 3), () => page([1, 2], 0), current);
assert.deepEqual(ids(previouslyEmpty), [1, 2, 3, 4]);
assert.equal(previouslyEmpty.olderBefore, 0);
