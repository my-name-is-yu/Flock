import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

const directory = mkdtempSync(join(tmpdir(), 'flock-memory-history-'));
try {
  const module = new URL('../assets/memory-history.js', import.meta.url);
  const fixture = join(directory, 'index.html');
  writeFileSync(fixture, `<!doctype html><html><body><script type="module">
    import { createMemoryHistory } from ${JSON.stringify(module.href)};
    const opened = [];
    const revisions = [{ source: { taskId: 1, runId: 'run' }, createdAt: 1, diffSummary: '-0 +1 lines',
      learning: { taskTitle: 'Verify this project', requestEventId: 3, reportEventId: 7, added: [], removed: [] } }];
    const history = createMemoryHistory(async () => ({ revisions, before: 0 }), id => opened.push(id), () => null);
    document.body.append(history);
    await Promise.resolve();
    const [request, report] = history.querySelectorAll('.memory-history-source button');
    request.click();
    report.click();
    document.documentElement.dataset.openedEvents = opened.join(',');
  </script></body></html>`);
  const output = execFileSync(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--allow-file-access-from-files', `--user-data-dir=${directory}`,
    '--virtual-time-budget=3000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
  assert.match(output, /<html[^>]*data-opened-events="3,7"/);
} finally {
  rmSync(directory, { recursive: true, force: true });
}
