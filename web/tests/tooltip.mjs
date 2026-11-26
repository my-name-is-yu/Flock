import { dumpDom, requirePass, withAssetServer } from './dom-harness.mjs';

async function exercise() {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const wait = (ms) => new Promise(resolve => setTimeout(resolve, ms));
  const pointer = (type, target, relatedTarget = null, pointerType = 'mouse') =>
    target.dispatchEvent(new PointerEvent(type, { bubbles: true, pointerType, relatedTarget }));
  const tip = () => document.querySelector('.tooltip');
  const open = () => !!tip()?.matches(':popover-open');
  const close = document.getElementById('close');
  const copy = document.getElementById('copy');
  const menu = document.getElementById('menu');

  pointer('pointerover', close);
  await wait(100);
  check(!open(), 'The tooltip appeared before the hover delay');
  await wait(500);
  check(open() && tip().textContent === 'CloseEsc' && tip().querySelector('kbd')?.textContent === 'Esc',
    'Hovering did not show the label with its shortcut');
  check(tip().getAttribute('aria-hidden') === 'true', 'The tooltip duplicated the accessible name');

  pointer('pointerout', close, copy);
  pointer('pointerover', copy);
  check(open() && tip().textContent === 'Copy text', 'Moving to a neighbor did not reuse the warm tooltip');
  copy.dataset.tooltip = 'Copied';
  await wait(0);
  check(tip().textContent === 'Copied', 'The tooltip did not follow a label change');

  pointer('pointerdown', copy);
  check(!open(), 'Pressing the control left the tooltip open');
  pointer('pointerout', copy, document.body);
  await wait(700);

  pointer('pointerover', menu);
  await wait(600);
  check(!open(), 'An open menu still showed its tooltip');

  const summary = document.querySelector('#more summary');
  pointer('pointerover', summary);
  await wait(600);
  check(open(), 'The menu button did not show its tooltip');
  document.getElementById('more').open = true;
  await wait(50);
  check(!open(), 'Opening the menu left its tooltip over it');
  pointer('pointerout', summary, document.body);
  await wait(700);

  pointer('pointerover', close, null, 'touch');
  await wait(600);
  check(!open(), 'Touch showed a tooltip');

  const time = document.getElementById('time');
  pointer('pointerover', time);
  await wait(600);
  check(open() && tip().textContent === 'Nov 26, 2026, 14:38', 'An accessible description did not show as the tooltip');
  pointer('pointerout', time, document.body);

  const long = document.getElementById('long');
  pointer('pointerover', long);
  await wait(600);
  const { top, bottom } = tip().getBoundingClientRect();
  check(open() && top >= 0 && bottom <= innerHeight, 'A long tooltip did not fit in the window');
  pointer('pointerout', long, document.body);
  await wait(700);

  const more = document.getElementById('more');
  more.open = false;
  summary.focus();
  check(open() && tip().textContent === 'More actions', 'Keyboard focus did not show the tooltip');
  more.open = true;
  await wait(50);
  check(!open(), 'Opening the menu from the keyboard left its tooltip over the menu');
  more.querySelector('button').focus();
  more.open = false;
  summary.focus();
  await wait(50);
  check(open() && tip().textContent === 'More actions', 'Closing the menu hid the tooltip of the control that took focus back');
  const rendered = document.createElement('details');
  rendered.open = true;
  document.body.append(rendered);
  await wait(50);
  check(open(), 'Another disclosure opening elsewhere hid the tooltip');
}

const index = `<!doctype html><html><head><link rel="stylesheet" href="/style.css"></head><body>
  <button id="close" aria-label="Close" data-tooltip="Close" data-shortcut="Esc">x</button>
  <button id="copy" aria-label="Copy text" data-tooltip="Copy text">c</button>
  <button id="menu" aria-haspopup="menu" aria-expanded="true" data-tooltip="Menu">m</button>
  <time id="time" aria-description="Nov 26, 2026, 14:38">14:38</time>
  <button id="long" aria-label="Session" aria-description="${'Check the release plan and report regressions. '.repeat(80)}">s</button>
  <details id="more"><summary aria-label="More actions" data-tooltip="More actions">…</summary><button>Find</button></details>
  <script type="module">
    import '/tooltip.js';
    try {
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script></body></html>`;
await withAssetServer({ '/': index }, async (origin) => {
  requirePass(await dumpDom(process.argv[2], `${origin}/`, { budget: 10000 }), 'The browser did not finish the tooltip checks');
});
