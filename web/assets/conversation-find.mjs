import { timelineMessages } from './event-history.mjs';

const pagesPerSearch = 4;
const matchesPerView = 5;
const dateFormat = new Intl.DateTimeFormat('en', { dateStyle: 'medium' });
const textFields = new Map([
  ['conversation_message', 'text'], ['conversation_reply', 'text'], ['request', 'text'],
  ['user_answer', 'text'], ['waiting', 'text'], ['report', 'report'], ['completed', 'text'],
]);

function snippetFor(event, pattern) {
  const text = event.data[textFields.get(event.kind)];
  if (typeof text !== 'string') return null;
  const match = pattern.exec(text);
  if (!match) return null;
  const before = Array.from(text.slice(0, match.index));
  const after = Array.from(text.slice(match.index + match[0].length));
  return {
    before: (before.length > 44 ? '…' : '') + before.slice(-44).join(''),
    match: match[0],
    after: after.slice(0, 70).join('') + (after.length > 70 ? '…' : ''),
  };
}

function element(tag, className, text = '') {
  const item = document.createElement(tag);
  item.className = className;
  item.textContent = text;
  return item;
}

export function mountConversationFind({ toggle, panel, readPage, describeAuthor, openMatch }) {
  const input = panel.querySelector('input');
  const previous = panel.querySelector('.find-previous');
  const next = panel.querySelector('.find-next');
  const closeButton = panel.querySelector('.find-close');
  const status = panel.querySelector('.find-status');
  const list = panel.querySelector('.find-results');
  const more = panel.querySelector('.find-more');

  let context = null;
  let origin = null;
  let search = null;
  let visibleMatches = [];

  function renderStatus() {
    const { matches = [], index = 0, busy, before, error, denied } = search ?? {};
    const count = matches.length;
    let announcement = '';
    if (busy) announcement = 'Searching…';
    else if (denied) announcement = 'Conversation unavailable';
    else if (error) announcement = 'Could not search messages';
    else if (count) announcement = `${index + 1} of ${count} ${count === 1 ? 'match' : 'matches'}${before ? ' so far' : ''}`;
    else if (search) announcement = before ? 'No matches so far' : 'No matches';
    if (status.textContent !== announcement) status.textContent = announcement;
  }

  function markCurrent(button) {
    list.querySelector('[aria-current]')?.removeAttribute('aria-current');
    if (!button) return;
    button.setAttribute('aria-current', 'true');
    const bounds = list.getBoundingClientRect();
    const row = button.getBoundingClientRect();
    if (row.top < bounds.top) list.scrollTop += row.top - bounds.top;
    else if (row.bottom > bounds.bottom) list.scrollTop += row.bottom - bounds.bottom;
  }

  function render() {
    const { matches = [], index = 0, busy, before, error, denied } = search ?? {};
    renderStatus();
    previous.disabled = next.disabled = matches.length < 2;
    more.hidden = !search || denied || (!before && !error);
    more.disabled = !!busy;
    more.textContent = error ? 'Retry search' : 'Search earlier messages';
    list.hidden = !matches.length;
    const start = Math.floor(index / matchesPerView) * matchesPerView;
    const displayed = matches.slice(start, start + matchesPerView);
    if (visibleMatches.length !== displayed.length || visibleMatches.some((item, i) => item !== displayed[i])) {
      const rows = displayed.map((result, offset) => matchRow(result, start + offset));
      list.replaceChildren(...rows);
      visibleMatches = displayed;
    }
    markCurrent(list.children[index - start]?.firstElementChild);
  }

  function matchRow(result, index) {
    const row = element('li', '');
    const button = element('button', 'find-match');
    button.type = 'button';
    const date = new Date(result.time * 1000);
    const time = element('time', '', dateFormat.format(date));
    time.dateTime = date.toISOString();
    const meta = element('span', 'find-match-meta');
    meta.append(element('span', 'find-match-name', result.author), time);
    const snippet = element('span', 'find-snippet');
    snippet.append(result.snippet.before, element('mark', '', result.snippet.match), result.snippet.after);
    button.append(meta, snippet);
    button.addEventListener('click', () => {
      search.index = index;
      markCurrent(button);
      renderStatus();
      void openMatch(context, result.id, search.controller.signal);
    });
    row.append(button);
    return row;
  }

  function cancelSearch() {
    clearTimeout(search?.timer);
    search?.controller.abort();
    search = null;
  }

  function close(restoreFocus) {
    if (panel.hidden) return;
    const returnFocus = restoreFocus && panel.contains(document.activeElement);
    cancelSearch();
    input.value = '';
    panel.hidden = true;
    toggle.setAttribute('aria-expanded', 'false');
    render();
    if (returnFocus) {
      const target = origin?.isConnected && origin.getClientRects().length ? origin : toggle;
      target.focus({ preventScroll: true });
    }
    origin = null;
  }

  function open() {
    if (!context || !toggle.getClientRects().length || document.querySelector('dialog[open]')) return false;
    if (panel.hidden) origin = document.activeElement;
    panel.hidden = false;
    toggle.setAttribute('aria-expanded', 'true');
    input.focus({ preventScroll: true });
    input.select();
    return true;
  }

  async function readBatch(active) {
    const { scope, controller, pattern, matches } = active;
    try {
      for (let page = 0; page < pagesPerSearch; page++) {
        const { events, olderBefore } = await readPage(scope, active.before, controller.signal);
        if (controller.signal.aborted) return;
        // Include the adjacent newer event so a report and its completion can straddle pages.
        const timeline = active.newerEvent ? [...events, active.newerEvent] : events;
        const messages = timelineMessages(timeline, time => dateFormat.format(time * 1000));
        for (const { event } of messages.reverse()) {
          if (event.id === active.newerEvent?.id) continue;
          const snippet = snippetFor(event, pattern);
          if (snippet) matches.push({ id: event.id, time: event.time, author: describeAuthor(scope, event), snippet });
        }
        if (events.length) active.newerEvent = events[0];
        active.before = olderBefore;
        if (!olderBefore) break;
      }
    } catch (error) {
      if (controller.signal.aborted) return;
      active.denied = [403, 404].includes(error.status);
      if (active.denied) matches.length = 0;
      active.error = true;
    }
    active.busy = false;
    if (search === active) render();
  }

  function query() {
    cancelSearch();
    const value = input.value.trim();
    if (value) {
      const pattern = new RegExp(value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'iu');
      const active = { scope: context, controller: new AbortController(), pattern, matches: [],
        index: 0, before: 0, busy: true };
      search = active;
      active.timer = setTimeout(() => void readBatch(active), 180);
    }
    render();
  }

  function step(direction) {
    if (!search?.matches.length) return;
    const count = search.matches.length;
    search.index = (search.index + direction + count) % count;
    render();
  }

  toggle.addEventListener('click', () => panel.hidden ? open() : close(false));
  closeButton.addEventListener('click', () => close(true));
  input.addEventListener('input', (event) => { if (!event.isComposing) query(); });
  input.addEventListener('compositionend', query);
  previous.addEventListener('click', () => step(-1));
  next.addEventListener('click', () => step(1));
  more.addEventListener('click', () => {
    if (!search || search.busy || search.denied) return;
    search.busy = true;
    search.error = false;
    input.focus({ preventScroll: true });
    render();
    void readBatch(search);
  });
  input.addEventListener('keydown', (event) => {
    if (event.isComposing || event.keyCode === 229) return;
    if (event.key === 'Enter') {
      event.preventDefault();
      list.querySelector('[aria-current]')?.click();
    } else if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
      if (!search?.matches.length) return;
      event.preventDefault();
      step(event.key === 'ArrowUp' ? -1 : 1);
    }
  });
  document.addEventListener('keydown', (event) => {
    if (event.defaultPrevented || event.isComposing || event.keyCode === 229) return;
    const shortcut = event.key.toLowerCase() === 'f' && (event.metaKey || event.ctrlKey) && !event.shiftKey && !event.altKey;
    if (shortcut && open()) event.preventDefault();
    if (event.key === 'Escape' && !panel.hidden && !document.querySelector('dialog[open]')) {
      event.preventDefault();
      if (input.value) {
        if (panel.contains(document.activeElement)) input.focus({ preventScroll: true });
        input.value = '';
        query();
      } else close(true);
    }
  });

  return {
    setContext(nextContext) {
      if (context === nextContext) return;
      close(false);
      context = nextContext;
      toggle.hidden = !context;
    },
  };
}
