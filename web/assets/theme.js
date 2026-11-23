// Loaded as a blocking classic script so a saved theme applies before the first paint.
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
  function storedMode() {
    let raw;
    try { raw = localStorage.getItem('flock-theme'); }
    catch { return; }
    try {
      const value = JSON.parse(raw);
      return modes.includes(value) ? value : 'system';
    } catch { return 'system'; }
  }
  function restoreStoredMode() {
    const stored = storedMode();
    if (stored && stored !== mode) document.dispatchEvent(new CustomEvent('flock-theme', { detail: stored }));
  }
  apply(storedMode());
  dark.addEventListener('change', () => apply(mode));
  document.addEventListener('flock-theme', (event) => apply(event.detail));
  window.addEventListener('pageshow', (event) => { if (event.persisted) restoreStoredMode(); });
  window.addEventListener('storage', (event) => {
    if (event.key === 'flock-theme' || event.key === null) restoreStoredMode();
  });
})();
