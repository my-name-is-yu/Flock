// Loaded as a blocking classic script so a saved theme applies before the first paint.
// app.js saves the choice and dispatches a `flock-theme` event to switch at runtime.
'use strict';
(() => {
  const root = document.documentElement;
  const dark = matchMedia('(prefers-color-scheme: dark)');
  const colors = { light: '#ffffff', dark: '#121212' };
  const modes = ['system', 'light', 'dark'];
  let mode = 'system';
  function apply(next) {
    mode = modes.includes(next) ? next : 'system';
    if (mode === 'system') delete root.dataset.theme;
    else root.dataset.theme = mode;
    const resolved = mode === 'system' ? (dark.matches ? 'dark' : 'light') : mode;
    document.querySelector('meta[name="theme-color"]')?.setAttribute('content', colors[resolved]);
  }
  let stored = null;
  try { stored = JSON.parse(localStorage.getItem('flock-theme')); }
  catch { /* Fall back to the system theme. */ }
  apply(stored);
  dark.addEventListener('change', () => apply(mode));
  document.addEventListener('flock-theme', (event) => apply(event.detail));
})();
