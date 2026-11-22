import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { refreshEventHistory, timelineMessages } from '../assets/event-history.mjs';
import { replyArrivalId } from '../assets/reply-arrival.mjs';

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

const oldMessage = { id: 2, kind: 'conversation_message', data: { status: 'needs_you' } };
const budgetReply = { id: 4, kind: 'conversation_reply', data: { replyToMessageId: 2, route: 'model_budget', status: 'needs_you' } };
const loaded = { events: new Map([oldMessage, budgetReply, ...page([90, 91, 100], 90).events].map(e => [e.id, e])),
  olderBefore: 2, newestEvent: 100 };
const latestAfterApproval = { ...page([100, 101], 100), changedMessageIds: [91, 92] };
const corrected = { ...oldMessage, data: { status: 'received' } };
const resolved = { ...budgetReply, data: { replyToMessageId: 2, status: 'done' } };
const delivered = { id: 91, kind: 'conversation_message', data: { deliveredToT3: true } };
await refreshEventHistory(loaded, latestAfterApproval, cursor => {
  if (cursor === 92) return { events: [delivered], olderBefore: 91 };
  if (cursor === 5) return { events: [{ id: 1 }, corrected, resolved], olderBefore: 0 };
  return noPage();
}, current);
assert.deepEqual(loaded.events.get(91), delivered);
assert.deepEqual(loaded.events.get(2), corrected);
assert.deepEqual(loaded.events.get(4), resolved);
assert.equal(loaded.events.has(1), false);
assert.equal(loaded.olderBefore, 2);
await refreshEventHistory(loaded, { ...latestAfterApproval, changedMessageIds: [] }, noPage, current);

const report = {
  id: 6, kind: 'report', time: 100, taskId: 1,
  data: { report: 'Full report', status: 'completed', truncated: false, revisionId: 2, resultRunId: 'result',
    run: { environmentId: 'env', threadId: 'thread', runId: 'launch' } },
};
const completion = {
  id: 8, kind: 'completed', time: 101, taskId: 1,
  data: { text: 'Full report', revisionId: 2, run: { environmentId: 'env', threadId: 'thread', runId: 'result' } },
};
const dayOf = time => Math.floor(time / 100);
const timeline = (...events) => timelineMessages(events, dayOf);
assert.deepEqual(timeline(report, completion), [{ event: completion, report }]);
for (const data of [
  { report: 'Full' }, { report: 'Full report ' }, { truncated: true }, { truncated: undefined },
  { status: 'failed' }, { status: undefined }, { revisionId: null }, { revisionId: 3 },
  { resultRunId: '' }, { resultRunId: 'launch' }, { run: null },
  { run: { ...report.data.run, environmentId: 'other' } },
  { run: { ...report.data.run, threadId: 'other' } },
]) {
  const distinct = { ...report, data: { ...report.data, ...data } };
  assert.deepEqual(timeline(distinct, completion), [{ event: distinct }, { event: completion }]);
}
for (const distinct of [{ ...completion, taskId: 2 }, { ...completion, time: 200 }]) {
  assert.deepEqual(timeline(report, distinct), [{ event: report }, { event: distinct }]);
}
assert.deepEqual(timeline(report, { kind: 'wait_closed' }, completion), [{ event: report }, { event: completion }]);
assert.deepEqual(timeline(completion), [{ event: completion }]);

const outgoing = (id) => ({ id, kind: 'conversation_message' });
const reply = (id) => ({ id, kind: 'conversation_reply' });
assert.equal(replyArrivalId([reply(4)], undefined), undefined, 'Initial history stays still');
assert.equal(replyArrivalId([reply(4), outgoing(5), reply(6)], 4), 6, 'The new reply follows its prompt');
assert.equal(replyArrivalId([reply(4), outgoing(5), reply(6)], 6), undefined, 'A duplicate refresh stays still');
assert.equal(replyArrivalId([reply(8), reply(9), reply(10)], 7), undefined, 'A large batch stays still');

const pairedReport = { id: 11, kind: 'report', taskId: 2 };
const complete = { id: 12, kind: 'completed', taskId: 2 };
const closed = { id: 13, kind: 'wait_closed' };
assert.equal(replyArrivalId([pairedReport, complete, closed], 10), 12, 'Only the visible completion in the pair animates');
assert.equal(replyArrivalId([pairedReport, complete, closed], 13), undefined, 'The completion does not replay');
assert.equal(replyArrivalId([pairedReport, { ...complete, taskId: 3 }], 10), undefined, 'Different requests stay still');
assert.equal(replyArrivalId([pairedReport, { ...complete, kind: 'report' }], 10), undefined, 'Two reports stay still');

const liveUpdatesSource = await readFile(new URL('../assets/live-updates.js', import.meta.url));
const { coalescedRefresh } = await import(`data:text/javascript;base64,${liveUpdatesSource.toString('base64')}`);
const refreshes = [];
const refresh = coalescedRefresh((scroll, generation) => refreshes.push([scroll, generation]));
void refresh(undefined, 0);
await refresh(undefined, 0);
void refresh(undefined, 1);
await refresh('bottom');
void refresh();
await refresh(undefined, 1);
void refresh(undefined, 1);
await refresh(undefined, 2);
assert.deepEqual(refreshes, [[undefined, 0], ['bottom', null], [undefined, null], [undefined, null]],
  'A coalesced refresh is live only when every trigger is live in the same visibility generation');
