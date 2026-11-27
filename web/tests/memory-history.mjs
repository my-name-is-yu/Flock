import assert from 'node:assert/strict';
import { assetUrl, dumpFixture } from './dom-harness.mjs';

const output = await dumpFixture(process.argv[2], `<!doctype html><html><body><script type="module">
  import { createMemoryHistory } from ${JSON.stringify(assetUrl('memory-history.js'))};
  const opened = [];
  const revisions = [{ author: 'bot:release', source: { taskId: 1, runId: 'run' }, createdAt: 1, diffSummary: '+1 line',
    learning: { taskTitle: 'Verify this project', requestEventId: 3, reportEventId: 7, added: [], removed: [] } }];
  const history = createMemoryHistory(async () => ({ revisions, before: 0 }), id => opened.push(id), () => null, 'Release Bot');
  document.body.append(history);
  await Promise.resolve();
  const [request, report] = history.querySelectorAll('.memory-history-source button');
  request.click();
  report.click();
  document.documentElement.dataset.openedEvents = opened.join(',');
</script></body></html>`);
assert.match(output, /Release Bot/);
assert.match(output, /<html[^>]*data-opened-events="3,7"/);
