import { assetUrl, dumpFixture, requirePass } from './dom-harness.mjs';

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
  const initial = draw('bot', [['first', 'First message']]);
  const first = timeline.firstElementChild;
  check(initial.length === 1 && initial[0] === first, 'The first row was not reported as added');
  const text = first.querySelector('p').firstChild;
  const copy = first.querySelector('button');
  copy.focus();
  window.getSelection().setBaseAndExtent(text, 0, text, 5);
  const entries = [['earlier', 'Earlier message'], ['first', 'First message'], ['newer', 'Newer message']];
  const added = draw('bot', entries);
  check(timeline.children[1] === first && document.activeElement === copy && window.getSelection().toString() === 'First',
    'A live append or history prepend replaced a focused message or cleared selection');
  check(added.length === 2 && added[0] === timeline.firstElementChild && added[1] === timeline.lastElementChild,
    'The prepend or append did not report exactly the newly mounted rows');
  check(draw('bot', entries).length === 0, 'Unchanged rows were reported as added');

  const replaced = draw('bot', [['first', 'Revised message']]);
  const revised = timeline.firstElementChild;
  check(replaced.length === 1 && replaced[0] === revised, 'A replacement row was not reported as added');
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

const html = `<!doctype html><html><body><section tabindex="0"><div></div></section><textarea></textarea>
  <script type="module">
    import { createTimelineRows } from ${JSON.stringify(assetUrl('timeline-rows.js'))};
    try {
      (${exercise})(createTimelineRows);
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script></body></html>`;
requirePass(await dumpFixture(process.argv[2], html), 'The browser did not finish the DOM checks');
