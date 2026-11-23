import assert from 'node:assert/strict';
import { matchTimeLabel } from '../assets/conversation-find.mjs';

const now = new Date(2026, 10, 23, 14, 0);
assert.equal(matchTimeLabel(new Date(2026, 10, 23, 9, 5), now), '09:05');
assert.equal(matchTimeLabel(new Date(2026, 10, 23, 0, 0), now), '00:00');
assert.equal(matchTimeLabel(new Date(2026, 10, 22, 23, 59), now), 'Yesterday');
assert.equal(matchTimeLabel(new Date(2026, 9, 2, 12), now), 'Oct 2');
assert.equal(matchTimeLabel(new Date(2025, 10, 23, 12), now), 'Nov 23, 2025');
// Across a new year, last week's results keep their year.
assert.equal(matchTimeLabel(new Date(2025, 11, 28, 12), new Date(2026, 0, 3, 9)), 'Dec 28, 2025');
