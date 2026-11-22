import assert from 'node:assert/strict';
import { refreshEventHistory } from '../assets/event-history.mjs';
import { createReplyArrival } from '../assets/reply-arrival.mjs';

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

const arrivals = [];
const showReply = createReplyArrival((id) => arrivals.push(id));
const conversation = { newestEvent: 4 };
const outgoing = (id) => ({ id, kind: 'conversation_message' });
const reply = (id) => ({ id, kind: 'conversation_reply' });
showReply(conversation, [reply(4)], true);
conversation.newestEvent = 6;
showReply(conversation, [reply(4), outgoing(5), reply(6)], true);
showReply(conversation, [reply(4), outgoing(5), reply(6)], true);
assert.deepEqual(arrivals, [6], 'Only the new live reply animates, once');

conversation.newestEvent = 7;
showReply(conversation, [reply(7)], false);
showReply(conversation, [reply(7)], true);
conversation.newestEvent = 10;
showReply(conversation, [reply(8), reply(9), reply(10)], true);
showReply({ newestEvent: 11 }, [reply(11)], true);
assert.deepEqual(arrivals, [6], 'Hidden, reduced-motion, backlog, and newly opened history stay still');
