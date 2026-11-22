import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

function exercise(createTimelineRows) {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const timeline = document.querySelector('div');
  const history = document.querySelector('section');
  const message = document.querySelector('textarea');
  const update = createTimelineRows(timeline, history);
  const draw = (scope, entries) => update(scope, (keep) => entries.map(([key, text, disabled]) =>
    keep(key, [text, disabled], () => {
      const row = document.createElement('article');
      row.tabIndex = -1;
      const body = document.createElement('p');
      body.textContent = text;
      const copy = document.createElement('button');
      copy.dataset.timelineAction = 'copy';
      copy.textContent = 'Copy text';
      copy.disabled = !!disabled;
      row.append(body, copy);
      return row;
    })));
  draw('bot', [['first', 'First message']]);
  const first = timeline.firstElementChild;
  const text = first.querySelector('p').firstChild;
  const copy = first.querySelector('button');
  copy.focus();
  window.getSelection().setBaseAndExtent(text, 0, text, 5);
  draw('bot', [['earlier', 'Earlier message'], ['first', 'First message'], ['newer', 'Newer message']]);
  check(timeline.children[1] === first && document.activeElement === copy && window.getSelection().toString() === 'First',
    'A live append or history prepend replaced a focused message or cleared selection');

  draw('bot', [['first', 'Revised message']]);
  const revised = timeline.firstElementChild;
  check(revised !== first && document.activeElement === revised.querySelector('button'),
    'A changed row did not restore the same available action');
  draw('bot', [['first', 'Revised message', true]]);
  check(document.activeElement === timeline.firstElementChild, 'A disabled action did not fall back to its message');
  draw('bot', []);
  check(document.activeElement === history, 'A removed row left focus outside the history');

  message.focus();
  draw('bot', [['first', 'Incoming message']]);
  check(document.activeElement === message, 'The first incoming message stole composer focus');
  const inBot = timeline.firstElementChild;
  draw('peer', [['first', 'Incoming message']]);
  check(timeline.firstElementChild !== inBot, 'A different conversation reused the previous conversation row');
}

const directory = mkdtempSync(join(tmpdir(), 'flock-timeline-rows-'));
try {
  const module = new URL('../assets/timeline-rows.js', import.meta.url);
  const html = `<!doctype html><html><body><section tabindex="0"><div></div></section><textarea></textarea>
    <script type="module">
      import { createTimelineRows } from ${JSON.stringify(module.href)};
      try {
        (${exercise})(createTimelineRows);
        document.documentElement.dataset.flockResult = 'pass';
      } catch (error) {
        document.documentElement.dataset.flockResult = error.message;
      }
    </script></body></html>`;
  const fixture = join(directory, 'index.html');
  writeFileSync(fixture, html);
  const output = execFileSync(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--allow-file-access-from-files', `--user-data-dir=${directory}`,
    '--virtual-time-budget=3000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
  const result = output.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the DOM checks');
} finally {
  rmSync(directory, { recursive: true, force: true });
}
