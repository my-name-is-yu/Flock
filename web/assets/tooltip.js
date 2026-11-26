// One tooltip for every control. Give an element data-tooltip="Label" and, for a
// keyboard shortcut, data-shortcut="Esc" (space-separated keys); data-tooltip-side="top"
// prefers the space above, and data-tooltip-anchor="selector" places it against
// the closest matching ancestor. Mouse hover
// shows it after a short delay, moving between controls keeps it warm, and
// keyboard focus shows it at once. Touch never shows it.
const showDelay = 450;
const warmPeriod = 600;
const gap = 6;
const margin = 8;

let tip = null;
let target = null;
let timer = 0;
let warmUntil = 0;
const watcher = new MutationObserver(() => {
  if (target?.isConnected && target.dataset.tooltip) render();
  else hide();
});

function ensureTip() {
  if (tip) return tip;
  tip = document.createElement('div');
  tip.className = 'tooltip';
  tip.setAttribute('popover', 'manual');
  tip.setAttribute('aria-hidden', 'true');
  document.body.append(tip);
  return tip;
}

function render() {
  const label = document.createElement('span');
  label.className = 'tooltip-label';
  label.textContent = target.dataset.tooltip;
  const keys = (target.dataset.shortcut ?? '').split(' ').filter(Boolean).map((key) => {
    const kbd = document.createElement('kbd');
    kbd.textContent = key;
    return kbd;
  });
  tip.replaceChildren(label, ...keys);
  place();
}

function place() {
  const anchor = (target.closest(target.dataset.tooltipAnchor || '*') ?? target).getBoundingClientRect();
  const { width, height } = tip.getBoundingClientRect();
  const below = anchor.bottom + gap;
  const above = anchor.top - gap - height;
  const preferAbove = target.dataset.tooltipSide === 'top' && above >= margin;
  const top = preferAbove || below + height + margin > innerHeight ? above : below;
  const centered = anchor.left + anchor.width / 2 - width / 2;
  const left = Math.min(Math.max(centered, margin), innerWidth - width - margin);
  tip.style.translate = `${Math.round(left)}px ${Math.round(top)}px`;
  tip.dataset.side = top < anchor.top ? 'top' : 'bottom';
}

function show(next) {
  clearTimeout(timer);
  // Skip controls whose menu or popup is already open.
  if (!next.isConnected || !next.dataset.tooltip || next.matches('[aria-haspopup][aria-expanded="true"], details[open] > summary'))
    return;
  target = next;
  ensureTip();
  // Reopening puts the tooltip above any dialog opened since it was last shown.
  if (tip.matches(':popover-open')) tip.hidePopover();
  tip.showPopover();
  render();
  watcher.disconnect();
  watcher.observe(target, { attributes: true, attributeFilter: ['data-tooltip', 'data-shortcut', 'hidden', 'aria-expanded'] });
}

function hide() {
  clearTimeout(timer);
  watcher.disconnect();
  if (target && tip?.matches(':popover-open')) {
    warmUntil = performance.now() + warmPeriod;
    tip.hidePopover();
  }
  target = null;
}

function owner(node) {
  return node instanceof Element ? node.closest('[data-tooltip]') : null;
}

document.addEventListener('pointerover', (event) => {
  if (event.pointerType === 'touch') return;
  const next = owner(event.target);
  if (!next || next === target) return;
  hide();
  if (performance.now() < warmUntil) show(next);
  else timer = setTimeout(() => show(next), showDelay);
});
document.addEventListener('pointerout', (event) => {
  const from = owner(event.target);
  if (from && !from.contains(event.relatedTarget)) {
    if (from === target) hide();
    else clearTimeout(timer);
  }
});
document.addEventListener('focusin', (event) => {
  const next = owner(event.target);
  if (next && next === event.target && next.matches(':focus-visible')) show(next);
});
document.addEventListener('focusout', (event) => {
  if (owner(event.target) === target) hide();
});
for (const type of ['pointerdown', 'wheel', 'scroll']) document.addEventListener(type, hide, { capture: true, passive: true });
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && target) hide();
}, { capture: true });
addEventListener('resize', hide);
addEventListener('blur', hide);
