import { presenceMotion, showAvatar } from './avatars.js';
import { stateLabels } from './conversation-copy.mjs';
import { node } from './dom.js';
import { placeRows } from './timeline-rows.js';
import {
  assignMenu,
  assignSettingsReturnFocus,
  botDraft,
  bots,
  byId,
  elements,
  listLoaded,
  menu,
  readState,
  reducedMotion,
  responseViewers,
  sidebar,
  sidebarIcons,
  sidebarViewer,
  view,
} from './app-state.js';
import { avatar, button, changed, save, selectBot } from './app-core.js';
import { openDetail } from './bot-detail.js';
import { botDeletion, botPause, decisions, pins } from './app.js';

const activePresence = new Set(['working', 'thinking', 'waiting', 'paused']);
const busyPresence = new Set(['working', 'thinking']);
const sidebarEntries = new Map();
function sidebarBots() {
  if (botDraft?.mode !== 'edit') return bots;
  return bots.map((bot) => (bot.id === botDraft.botId ? { ...bot, name: botDraft.name, avatar: botDraft.avatar } : bot));
}
export function pinnedBotsInShortcutOrder(source = bots) {
  return [...pins].map((id) => source.find((bot) => bot.id === id)).filter(Boolean);
}
export function focusKey(key) {
  sidebar.querySelector(`[data-focus="${CSS.escape(key)}"]`)?.focus();
}
function positionMenuPopover(botId) {
  if (!botId) return;
  const popover = sidebar.querySelector('.menu-popover');
  const trigger = sidebar.querySelector(`[data-focus="${CSS.escape(`menu:${botId}`)}"]`);
  if (!popover || !trigger) return;
  const triggerRect = trigger.getBoundingClientRect();
  const width = popover.offsetWidth;
  const height = popover.offsetHeight;
  const margin = 8;
  const viewLeft = Math.max(margin, Math.min(window.innerWidth - width - margin, triggerRect.right - width));
  const fitsBelow = triggerRect.bottom + 4 + height <= window.innerHeight - margin;
  const viewTop = fitsBelow
    ? triggerRect.bottom + 4
    : Math.max(margin, triggerRect.top - height - 4);
  const sidebarRect = getComputedStyle(sidebar).transform !== 'none' ? sidebar.getBoundingClientRect() : null;
  const offsetX = sidebarRect ? sidebarRect.left : 0;
  const offsetY = sidebarRect ? sidebarRect.top : 0;
  popover.style.left = `${Math.round(viewLeft - offsetX)}px`;
  popover.style.top = `${Math.round(viewTop - offsetY)}px`;
  popover.style.setProperty('--menu-origin', fitsBelow ? 'top right' : 'bottom right');
}
function setMenu(botId, focus) {
  assignMenu(botId);
  renderBots();
  if (focus === 'first' || focus === 'last') {
    const items = sidebar.querySelectorAll('.menu-popover [role="menuitem"]');
    items[focus === 'first' ? 0 : items.length - 1]?.focus();
  } else if (focus) focusKey(focus);
}
export function closeMenu(restore = true) {
  const botId = menu;
  if (!botId) return;
  setMenu(null, restore ? `menu:${botId}` : null);
}
function togglePin(botId) {
  if (pins.has(botId)) pins.delete(botId);
  else pins.add(botId);
  save('flock-pins', [...pins]);
  setMenu(null, `menu:${botId}`);
}
export function openSettingsFromMenu(botId) {
  assignSettingsReturnFocus(botId);
  openDetail({ kind: 'settings' });
}
export function openBotSettings(botId) {
  assignMenu(null);
  if (view?.id !== botId) selectBot(botId);
  else renderBots();
  if (view?.data) openSettingsFromMenu(botId);
  else if (view) view.openSettingsOnLoad = botId;
}
function menuButton(botId) {
  const toggle = button(null, 'row-menu', () => (menu === botId ? closeMenu() : setMenu(botId, 'first')));
  toggle.innerHTML = sidebarIcons.more;
  toggle.dataset.focus = `menu:${botId}`;
  toggle.setAttribute('aria-haspopup', 'menu');
  toggle.addEventListener('keydown', (event) => {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    event.preventDefault();
    setMenu(botId, event.key === 'ArrowDown' ? 'first' : 'last');
  });
  return toggle;
}
function menuPopover(bot) {
  const list = node('div', 'menu-popover');
  list.id = 'bot-menu';
  list.setAttribute('role', 'menu');
  list.setAttribute('aria-label', `${bot.name} menu`);
  const actions = [
    [pins.has(bot.id) ? 'Unpin' : 'Pin', sidebarIcons.pin, () => togglePin(bot.id)],
    ['History', sidebarIcons.inbox, () => {
      selectBot(bot.id);
      void decisions.openHistory(sidebar.querySelector(`[data-focus="${CSS.escape(`menu:${bot.id}`)}"]`));
    }],
    ['Bot settings', sidebarIcons.settings, () => openBotSettings(bot.id)],
  ];
  if (bot.canDelete) {
    actions.push([bot.paused ? 'Resume…' : 'Pause…', bot.paused ? sidebarIcons.resume : sidebarIcons.pause, () => {
      closeMenu();
      botPause.open(bot, () => focusKey(`menu:${bot.id}`));
    }]);
  }
  const menuItem = ([label, icon, run], index) => {
    const item = button(null, null, run);
    item.innerHTML = icon;
    item.append(node('span', null, label));
    item.setAttribute('role', 'menuitem');
    item.tabIndex = -1;
    item.dataset.focus = `item:${bot.id}:${index}`;
    return item;
  };
  list.append(...actions.map(menuItem));
  if (bot.canDelete) {
    const remove = menuItem(['Delete…', sidebarIcons.trash, () => {
      closeMenu();
      botDeletion.open(bots.find(({ id }) => id === bot.id), document.activeElement);
    }], actions.length);
    remove.classList.add('danger');
    remove.setAttribute('aria-haspopup', 'dialog');
    list.append(remove);
  }
  list.addEventListener('keydown', (event) => {
    const items = [...list.querySelectorAll('[role="menuitem"]')];
    const index = items.indexOf(document.activeElement);
    const moves = { ArrowDown: index + 1, ArrowUp: index - 1, Home: 0, End: items.length - 1 };
    if (event.key in moves) {
      event.preventDefault();
      items[(moves[event.key] + items.length) % items.length].focus();
    } else if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      closeMenu();
    } else if (event.key === 'Tab') closeMenu();
  });
  return list;
}
function createSidebarEntry(botId) {
  const row = node('div');
  const select = button(null, '', () => {
    decisions.close({ focus: false });
    selectBot(botId);
  });
  select.dataset.focus = `bot:${botId}`;
  const face = node('span');
  const avatarHost = node('span', 'avatar');
  const pinPresence = node('span');
  pinPresence.innerHTML = sidebarIcons.pause;
  pinPresence.setAttribute('aria-hidden', 'true');
  const pinUnread = node('span', 'unread-dot pin-unread');
  pinUnread.setAttribute('aria-hidden', 'true');
  face.append(avatarHost, pinPresence, pinUnread);

  const name = node('span', 'bot-name');
  const nameText = node('span', 'bot-name-text');
  const presenceDot = node('span', 'presence-dot');
  const rowUnread = node('span', 'unread-dot row-unread');
  rowUnread.setAttribute('aria-hidden', 'true');
  name.append(nameText);
  const preview = node('span', 'bot-preview');
  const dots = node('span', 'working-dots');
  dots.setAttribute('aria-hidden', 'true');
  dots.append(node('i'), node('i'), node('i'));
  const previewText = document.createTextNode('');
  preview.append(presenceDot, dots, previewText);
  const text = node('span', 'bot-text');
  text.append(name, preview);

  const pinName = node('span', 'pin-name');
  const status = node('span', 'sr-only');
  select.append(rowUnread, face, text, pinName, status);
  const toggle = menuButton(botId);
  const decisionCount = button(null, 'decision-count', () => {
    selectBot(botId);
    decisions.open(decisionCount);
  });
  decisionCount.dataset.focus = `decisions:${botId}`;
  decisionCount.setAttribute('aria-controls', 'decision-panel');
  row.append(select, decisionCount, toggle);
  row.addEventListener('contextmenu', (event) => {
    event.preventDefault();
    setMenu(botId, 'first');
  });

  let previousBot;
  let avatarHandle;
  let popover;
  return {
    row,
    update(bot, pinIndex) {
      const pinned = pinIndex >= 0;
      const active = view?.id === botId;
      const expanded = menu === botId;
      row.className = pinned ? 'pin-tile' : 'bot-row';
      row.classList.toggle('selected', active);
      row.classList.toggle('menu-open', expanded);
      select.className = pinned ? 'pin' : 'bot-select';
      if (active) select.setAttribute('aria-current', 'true');
      else select.removeAttribute('aria-current');
      const count = decisions.count(botId);
      decisionCount.hidden = !count;
      decisionCount.textContent = count;
      decisionCount.setAttribute('aria-label', `${bot.name}: ${count} ${count === 1 ? 'item needs' : 'items need'} you`);

      face.className = pinned ? 'pin-face' : '';
      avatarHost.classList.toggle('pin-avatar', pinned);
      avatarHost.classList.toggle('bot-avatar', !pinned);
      pinName.hidden = !pinned;
      text.hidden = pinned;
      const shortcut = pinned && pinIndex < 9 ? pinIndex + 1 : null;
      if (shortcut) {
        select.dataset.shortcut = `⌥⇧${shortcut}`;
        select.setAttribute('aria-keyshortcuts', `Alt+Shift+${shortcut}`);
      } else {
        delete select.dataset.shortcut;
        select.removeAttribute('aria-keyshortcuts');
      }

      if (previousBot?.name !== bot.name) {
        nameText.textContent = pinName.textContent = bot.name;
        toggle.setAttribute('aria-label', `${bot.name} menu`);
        popover?.setAttribute('aria-label', `${bot.name} menu`);
      }
      const presence = bot.paused ? 'paused' : bot.presence;
      if (pinned) select.dataset.tooltip = bot.paused ? `${bot.name} · Paused` : bot.name;
      else delete select.dataset.tooltip;
      const showWaitingPreview = presence === 'waiting' && !count;
      const previewLabel = showWaitingPreview || bot.paused ? stateLabels[presence] : bot.preview || '';
      if (previewText.data !== previewLabel) previewText.data = previewLabel;
      preview.classList.toggle('waiting', showWaitingPreview);
      if (showWaitingPreview || bot.paused) preview.setAttribute('aria-hidden', 'true');
      else preview.removeAttribute('aria-hidden');
      const hasPresence = activePresence.has(presence);
      presenceDot.hidden = !showWaitingPreview;
      dots.hidden = !busyPresence.has(presence);
      preview.hidden = !previewLabel && dots.hidden;
      pinPresence.hidden = !pinned || !hasPresence;
      pinPresence.className = hasPresence ? `pin-presence ${presence}` : 'pin-presence';
      const unread = hasUnread(bot);
      row.classList.toggle('unread', !!unread);
      pinUnread.hidden = !pinned || !unread;
      rowUnread.hidden = pinned || !unread;
      const label = [unread && 'New reply', hasPresence && stateLabels[presence]].filter(Boolean).join(', ');
      status.hidden = !label;
      if (status.textContent !== label) status.textContent = label;

      toggle.setAttribute('aria-expanded', expanded);
      if (expanded) {
        toggle.setAttribute('aria-controls', 'bot-menu');
        if (popover && (previousBot?.canDelete !== bot.canDelete || previousBot?.paused !== bot.paused)) {
          if (popover.contains(document.activeElement)) toggle.focus();
          popover.remove();
          popover = null;
        }
        if (!popover) {
          popover = menuPopover(bot);
          row.append(popover);
        }
      } else if (popover) {
        toggle.removeAttribute('aria-controls');
        popover.remove();
        popover = null;
      }
      if (!previousBot || previousBot.paused !== bot.paused || JSON.stringify(previousBot.avatar) !== JSON.stringify(bot.avatar)) {
        avatarHandle?.destroy();
        avatarHandle = showAvatar(avatarHost, bot, { still: !!bot.paused, state: presenceMotion(bot.paused ? 'idle' : bot.presence) });
      } else if (previousBot.presence !== bot.presence) {
        const nextMotion = presenceMotion(bot.presence);
        if (presenceMotion(previousBot.presence) !== nextMotion) avatarHandle?.update({ state: nextMotion });
      }
      previousBot = bot;
    },
    destroy() {
      avatarHandle?.destroy();
      row.remove();
    },
  };
}
export function skeletonRows(count, className = 'skeleton-row') {
  return Array.from({ length: count }, () => {
    const row = node('div', className);
    row.setAttribute('aria-hidden', 'true');
    if (className === 'skeleton-bot') row.append(node('span', 'skeleton-avatar'));
    const lines = node('span', 'skeleton-lines');
    lines.append(node('span', 'skeleton-line'), node('span', 'skeleton-line short'));
    row.append(lines);
    return row;
  });
}

function draftRow(draft) {
  const row = node('div', 'bot-row draft');
  row.setAttribute('aria-hidden', 'true');
  const select = node('span', 'bot-select');
  const name = node('span', 'bot-name');
  name.append(node('span', 'bot-name-text', draft.name));
  const text = node('span', 'bot-text');
  text.append(name);
  select.append(avatar(draft, 'bot-avatar'), text);
  row.append(select);
  return row;
}

const rowRevealTimers = new WeakMap();
export function triggerRowReveal(container) {
  if (reducedMotion.matches) return;
  clearTimeout(rowRevealTimers.get(container));
  container.classList.remove('rows-reveal');
  void container.offsetWidth;
  container.classList.add('rows-reveal');
  rowRevealTimers.set(container, setTimeout(() => {
    container.classList.remove('rows-reveal');
    rowRevealTimers.delete(container);
  }, 400));
}
function clearRowReveal(container) {
  clearTimeout(rowRevealTimers.get(container));
  rowRevealTimers.delete(container);
  container.classList.remove('rows-reveal');
}

let previousPinsKey = null;
let previousListLoaded = false;
export function renderBots() {
  const unreadIds = bots.filter(hasUnread).map((bot) => bot.id);
  if (!changed('bots', bots, [...pins], menu, view?.id, listLoaded, botDraft, unreadIds,
    bots.map(({ id }) => decisions.count(id)))) return;
  const revealedFromSkeleton = !previousListLoaded && listLoaded;
  previousListLoaded = listLoaded;
  const pinsKey = JSON.stringify([...pins]);
  const animateFlip = previousPinsKey !== null && previousPinsKey !== pinsKey && !reducedMotion.matches;
  previousPinsKey = pinsKey;
  if (animateFlip || menu) {
    clearRowReveal(elements['bot-list']);
    clearRowReveal(elements.pinned);
  }
  const beforeRects = new Map();
  if (animateFlip) {
    for (const entry of sidebarEntries.values()) {
      if (entry.row.isConnected) beforeRects.set(entry.row, entry.row.getBoundingClientRect());
    }
    if (!elements['bot-activity'].hidden) {
      beforeRects.set(elements['bot-activity'], elements['bot-activity'].getBoundingClientRect());
    }
  }
  const focusedSidebarKey = sidebar.contains(document.activeElement) ? document.activeElement.dataset.focus : null;
  const source = sidebarBots();
  const pinned = pinnedBotsInShortcutOrder(source);
  const unpinned = source.filter((bot) => !pins.has(bot.id));
  const activeIds = new Set(source.map((bot) => bot.id));
  for (const [id, entry] of sidebarEntries) {
    if (!activeIds.has(id)) {
      entry.destroy();
      sidebarEntries.delete(id);
    }
  }
  for (const { id } of source) {
    if (!sidebarEntries.has(id)) sidebarEntries.set(id, createSidebarEntry(id));
  }
  const rowFor = ({ id }) => sidebarEntries.get(id).row;
  placeRows(elements.pinned, pinned.map(rowFor));
  elements.pinned.hidden = !pinned.length;
  const list = elements['bot-list'];
  list.setAttribute('aria-busy', !listLoaded);
  const rows = unpinned.map(rowFor);
  if (botDraft?.mode === 'create') rows.unshift(draftRow(botDraft));
  if (!listLoaded) rows.push(...skeletonRows(4, 'skeleton-bot'));
  placeRows(list, rows);
  pinned.forEach((bot, index) => sidebarEntries.get(bot.id).update(bot, index));
  unpinned.forEach((bot) => sidebarEntries.get(bot.id).update(bot, -1));
  if (revealedFromSkeleton) {
    if (unpinned.length) triggerRowReveal(list);
    if (pinned.length) triggerRowReveal(elements.pinned);
  }
  if (animateFlip) {
    for (const [el, before] of beforeRects) {
      if (!el.isConnected || el.hidden) continue;
      const after = el.getBoundingClientRect();
      if (!after.width || !after.height) continue;
      const dx = before.left - after.left;
      const dy = before.top - after.top;
      if (Math.abs(dx) > 1 || Math.abs(dy) > 1) {
        el.animate?.([
          { transform: `translate(${dx}px, ${dy}px)` },
          { transform: 'none' },
        ], { duration: 230, easing: 'cubic-bezier(.2, .9, .25, 1)' });
      }
    }
  }
  if (menu) positionMenuPopover(menu);
  if (focusedSidebarKey && document.activeElement.dataset.focus !== focusedSidebarKey) focusKey(focusedSidebarKey);
}

function hasUnread(bot) {
  const state = readState.get(bot.id);
  return state && state.incoming > state.readThrough;
}

let readCheckFrame;
export function scheduleReadCheck() {
  readCheckFrame ??= requestAnimationFrame(() => {
    readCheckFrame = undefined;
    if (!view?.data || sidebarViewer === null || responseViewers.get(view.data) !== sidebarViewer ||
        document.hidden || decisions.isOpen() || document.querySelector('dialog[open]')) return;
    const state = readState.get(view.id);
    if (!state || state.incoming <= state.readThrough) return;
    const scroller = elements.conversation;
    if (!scroller.clientHeight) return;
    const message = byId(`event-${state.incoming}`);
    if (!message || !elements.timeline.contains(message)) return;
    const viewport = scroller.getBoundingClientRect();
    const bounds = message.getBoundingClientRect();
    if (bounds.bottom <= viewport.top || bounds.top >= viewport.bottom || bounds.bottom > viewport.bottom + 1) return;
    state.readThrough = state.incoming;
    renderBots();
  });
}
