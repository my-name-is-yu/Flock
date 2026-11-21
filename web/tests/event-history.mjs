import assert from 'node:assert/strict';
import { collectEventPages, mergeEventPages } from '../assets/event-history.mjs';

const page = (ids, olderBefore) => ({ events: ids.map(id => ({ id })), olderBefore });
const latest = page([9, 10], 9);
const older = new Map([[9, page([7, 8], 7)], [7, page([4, 5], 4)]]);
const current = () => true;
const noPage = () => assert.fail('unexpected history request');
const history = { events: new Map(), olderBefore: 0 };
const first = await collectEventPages(page([3, 4], 3), history.newestEvent, noPage, current);
mergeEventPages(history, first);
assert.equal(history.newestEvent, 4);
assert.equal(history.olderBefore, 3);

const overlap = await collectEventPages(page([4, 5], 4), history.newestEvent, noPage, current);
mergeEventPages(history, overlap);
assert.equal(history.newestEvent, 5);
assert.equal(history.olderBefore, 3);

const requested = [];
const pages = await collectEventPages(latest, history.newestEvent, before => {
  requested.push(before);
  return older.get(before);
}, current);
assert.deepEqual(requested, [9, 7]);
history.events.set(1, { id: 1 });
history.olderBefore = 0;
mergeEventPages(history, pages);
assert.deepEqual([...history.events.keys()].sort((a, b) => a - b), [1, 3, 4, 5, 7, 8, 9, 10]);
assert.equal(history.newestEvent, 10);
assert.equal(history.olderBefore, 0);

await assert.rejects(collectEventPages(latest, 4, () => latest, current), /Invalid conversation cursor/);
await assert.rejects(collectEventPages(latest, 4, before => {
  if (before === 7) throw Error('interrupted');
  return older.get(before);
}, current), /interrupted/);
let selected = true;
const stale = await collectEventPages(latest, 4, before => {
  selected = false;
  return older.get(before);
}, () => selected);
assert.equal(stale, null);
