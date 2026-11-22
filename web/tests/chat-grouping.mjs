import assert from 'node:assert/strict';
import { messagePlacement } from '../assets/chat-grouping.mjs';

const localTime = (hour, minute, second = 0) => new Date(2026, 1, 15, hour, minute, second).getTime() / 1000;
const message = (time, kind = 'conversation_message', taskId = 0, data = {}) => ({ time, kind, taskId, data });
const first = message(localTime(10, 0));
const second = message(localTime(10, 14, 59));
const later = message(localTime(10, 29, 59));

assert.equal(messagePlacement(null, first, second).groupStart, true);
assert.equal(messagePlacement(null, first, second).groupEnd, false);
assert.equal(messagePlacement(first, second, later).groupStart, false);
assert.equal(messagePlacement(first, second, later).groupEnd, true);
assert.equal(messagePlacement(first, second, later).timeBreak, false);
assert.equal(messagePlacement(second, later, null).groupStart, true);
assert.equal(messagePlacement(second, later, null).timeBreak, true);

const lastNight = message(localTime(23, 59));
const midnight = message(localTime(24, 0));
assert.equal(messagePlacement(null, lastNight, midnight).groupEnd, true);
assert.equal(messagePlacement(lastNight, midnight, null).groupStart, true);
assert.equal(messagePlacement(lastNight, midnight, null).timeBreak, false);
assert.equal(messagePlacement(second, first, null).groupStart, true);

const shortlyAfter = localTime(10, 1);
for (const boundary of [message(shortlyAfter, 'conversation_reply'), message(shortlyAfter, 'cancelled'),
  message(shortlyAfter, 'bot_message'), message(shortlyAfter, 'request', 4)]) {
  assert.equal(messagePlacement(null, first, boundary).groupEnd, true);
  assert.equal(messagePlacement(first, boundary, null).groupStart, true);
}

const question = message(localTime(10, 0), 'waiting', 4, { id: 2 });
const answer = message(shortlyAfter, 'user_answer', 4, { waitId: 2 });
assert.equal(messagePlacement(question, answer, null).taskStart, false);
assert.equal(messagePlacement(question, answer, null).afterQuestion, true);
