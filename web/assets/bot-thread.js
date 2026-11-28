import { showAvatar } from './avatars.js';
import { botDmPlacement } from './chat-grouping.mjs';
import { node, setMessageBody } from './dom.js';
import { refreshEventHistory } from './event-history.mjs';
import { createTimelineRows } from './timeline-rows.js';

export function botThreadUrl(bot, peer, message) {
  const params = new URLSearchParams({ bot, thread: peer });
  if (message) params.set('message', message);
  return `/?${params}`;
}

const clock = new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit' });
const day = new Intl.DateTimeFormat(undefined, { dateStyle: 'long' });
const fullTime = new Intl.DateTimeFormat(undefined, { dateStyle: 'long', timeStyle: 'short' });
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
const openMotion = [{ opacity: 0, transform: 'translateY(12px) scale(.985)' }, { opacity: 1, transform: 'none' }];

function avatar(participant, className) {
  const face = node('span', `avatar ${className}`);
  showAvatar(face, participant);
  return face;
}

function timestamp(date) {
  const time = node('time', 'msg-time', clock.format(date));
  time.dateTime = date.toISOString();
  time.setAttribute('aria-label', fullTime.format(date));
  return time;
}

export function mountBotThread() {
  const byId = (id) => document.getElementById(id);
  const main = byId('main');
  const panel = byId('bot-thread');
  const status = byId('bot-thread-status');
  const older = byId('bot-thread-older');
  const timeline = byId('bot-thread-timeline');
  const latest = byId('bot-thread-latest');
  const close = byId('bot-thread-close');
  const crumb = byId('profile-thread');
  const heading = byId('profile-heading');
  const updateRows = createTimelineRows(timeline, panel);
  let state = null;
  let poll = 0;
  let motion = null;

  function animate(element, keyframes, options) {
    if (reducedMotion.matches) return null;
    return element.animate(keyframes, { fill: 'both', ...options });
  }

  function readPage(before) {
    const params = new URLSearchParams({ bot: state.bot, peer: state.peer });
    if (before) params.set('before', before);
    else if (state.anchor) params.set('anchor', state.anchor);
    const current = state;
    return fetch(`/api/bot-dms?${params}`).then(async response => {
      if (current !== state) throw Object.assign(new Error('stale'), { stale: true });
      if (!response.ok) throw Object.assign(new Error('Could not load Bot messages'), { status: response.status });
      return response.json();
    });
  }

  function setCrumb(text) {
    crumb.textContent = text;
    crumb.hidden = !text;
    if (text) heading.setAttribute('aria-labelledby', 'profile-name profile-thread');
    else heading.setAttribute('aria-labelledby', 'profile-name');
  }

  function drawMessage(event, sender, place) {
    const own = sender.id === state.bot;
    const row = node('article', `bot-thread-message${own ? ' bot-thread-own' : ''}`);
    row.id = `bot-thread-message-${event.id}`;
    row.tabIndex = -1;
    row.classList.toggle('bot-thread-anchored', String(event.id) === state.anchor);
    row.classList.toggle('group-start', place.groupStart);
    const meta = node('div', place.groupStart ? 'bot-thread-meta' : 'sr-only');
    const name = node('span', 'bot-thread-name', sender.name);
    name.dir = 'auto';
    meta.append(name, timestamp(new Date(event.time * 1000)));
    const body = node('div', 'bubble');
    const text = node('div', 'event-text');
    text.dir = 'auto';
    setMessageBody(text, event.data.textHtml, event.data.text);
    body.append(text);
    if (place.groupStart) row.append(avatar(sender, 'bot-thread-avatar'));
    row.append(meta, body);
    return row;
  }

  function drawTranscript() {
    const transcript = state.transcript;
    const events = [...transcript.events.values()].sort((a, b) => a.id - b.id);
    updateRows(transcript, keep => {
      const rows = [];
      let previousDay;
      for (const [index, event] of events.entries()) {
        const dateLabel = day.format(new Date(event.time * 1000));
        if (dateLabel !== previousDay) {
          rows.push(keep(`day-${event.id}`, dateLabel, () => node('p', 'day', dateLabel)));
          previousDay = dateLabel;
        }
        const sender = state.participants.find(({ id }) => id === event.data.fromBotId);
        const place = botDmPlacement(events[index - 1], event, events[index + 1]);
        rows.push(keep(String(event.id), [event, sender, place], () => drawMessage(event, sender, place)));
      }
      return rows;
    });
    timeline.hidden = !events.length;
    older.hidden = !transcript.olderBefore;
    status.textContent = events.length ? '' : 'No messages yet.';
  }

  function showError(error) {
    if (error.stale) return;
    if ([400, 403, 404].includes(error.status)) {
      state.transcript = { events: new Map(), olderBefore: 0 };
      updateRows(state.transcript, () => []);
      timeline.hidden = older.hidden = true;
      latest.hidden = !(state.anchor && error.status === 404);
      status.textContent = 'These Bot messages aren’t available.';
    } else {
      status.textContent = 'Couldn’t load Bot messages. Trying again…';
    }
  }

  async function refresh() {
    if (!state || state.pending) return;
    const current = state;
    current.pending = true;
    try {
      const page = await readPage();
      if (current !== state) return;
      state.participants = [page.bot, page.peer];
      setCrumb(page.peer.name);
      panel.setAttribute('aria-label', `Messages between ${page.bot.name} and ${page.peer.name}`);
      const first = state.transcript.newestEvent == null;
      const atEnd = panel.scrollHeight - panel.scrollTop - panel.clientHeight < 65;
      await refreshEventHistory(state.transcript, page, readPage, () => current === state);
      if (current !== state) return;
      drawTranscript();
      latest.hidden = !page.hasNewer;
      if (first && state.anchor) {
        const target = byId(`bot-thread-message-${state.anchor}`);
        target?.scrollIntoView({ block: 'center' });
        target?.focus({ preventScroll: true, focusVisible: false });
      } else if (first || (!state.anchor && atEnd)) panel.scrollTop = panel.scrollHeight;
    } catch (error) {
      if (current === state) showError(error);
    } finally {
      current.pending = false;
    }
  }

  function open(bot, peer, anchor) {
    if (state && state.bot === bot && state.peer === peer && state.anchor === anchor) return;
    const wasOpen = !!state;
    state = { bot, peer, anchor, participants: [], transcript: { events: new Map(), olderBefore: 0 } };
    updateRows(state.transcript, () => []);
    timeline.hidden = older.hidden = latest.hidden = true;
    status.textContent = 'Loading…';
    panel.setAttribute('aria-label', 'Bot messages');
    if (!wasOpen) {
      motion?.cancel();
      panel.hidden = close.hidden = false;
      main.classList.add('bot-thread-open');
      poll = setInterval(() => { if (!document.hidden) void refresh(); }, 5000);
      panel.focus({ preventScroll: true, focusVisible: false });
      // The conversation stays visible underneath until the thread has faded in over it.
      motion = animate(panel, openMotion, { duration: 240, easing: 'cubic-bezier(0.22, 1, 0.36, 1)' });
      animate(crumb, [{ opacity: 0, transform: 'translateX(-4px)' }, { opacity: 1, transform: 'none' }], { duration: 200, easing: 'ease-out' });
      const covered = () => { if (state) main.classList.add('bot-thread-covered'); };
      if (motion) motion.finished.then(covered, () => {});
      else covered();
    }
    void refresh();
  }

  function hide() {
    if (!state) return;
    state = null;
    clearInterval(poll);
    close.hidden = true;
    main.classList.remove('bot-thread-open', 'bot-thread-covered');
    setCrumb('');
    motion?.cancel();
    motion = animate(panel, [...openMotion].reverse(), { duration: 180, easing: 'cubic-bezier(0.4, 0, 1, 1)' });
    const hidden = () => {
      if (state) return;
      motion?.cancel();
      panel.hidden = true;
    };
    if (motion) motion.finished.then(hidden, () => {});
    else hidden();
  }

  function sync(botId) {
    const params = new URLSearchParams(location.search);
    if (botId && params.get('bot') === botId && params.get('thread')) {
      open(botId, params.get('thread'), params.get('message') ?? undefined);
    } else hide();
  }

  function dismiss() {
    if (!state) return;
    if (history.state?.botThread) {
      history.back();
      return;
    }
    const bot = state.bot;
    history.replaceState(null, '', `/?${new URLSearchParams({ bot })}`);
    sync(bot);
  }

  function navigate(href) {
    const url = new URL(href, location.href);
    const replace = !!state;
    if (replace) history.replaceState(history.state, '', url);
    else history.pushState({ botThread: true }, '', url);
    sync(url.searchParams.get('bot'));
  }

  document.addEventListener('click', event => {
    const link = event.target.closest?.('a[data-bot-thread]');
    if (!link || event.defaultPrevented || event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
    event.preventDefault();
    navigate(link.href);
  });
  older.addEventListener('click', async () => {
    const current = state;
    if (!current || current.pending || !current.transcript.olderBefore) return;
    current.pending = true;
    older.disabled = true;
    try {
      const page = await readPage(current.transcript.olderBefore);
      if (current !== state) return;
      const top = panel.scrollTop;
      const height = panel.scrollHeight;
      for (const event of page.events) current.transcript.events.set(event.id, event);
      current.transcript.olderBefore = page.olderBefore;
      drawTranscript();
      panel.scrollTop = top + panel.scrollHeight - height;
      if (older.hidden) panel.focus({ preventScroll: true });
    } catch (error) {
      if (current === state) showError(error);
    } finally {
      current.pending = false;
      older.disabled = false;
    }
  });
  latest.addEventListener('click', () => {
    if (state) navigate(botThreadUrl(state.bot, state.peer));
  });
  close.addEventListener('click', dismiss);
  panel.addEventListener('click', event => {
    if (window.getSelection()?.toString() || event.target.closest('.bubble, a, button, .bot-thread-meta, .bot-thread-avatar')) return;
    dismiss();
  });
  panel.addEventListener('keydown', event => {
    if (event.key === 'Escape' && !event.defaultPrevented) {
      event.preventDefault();
      dismiss();
    }
  });
  document.addEventListener('visibilitychange', () => { if (!document.hidden) void refresh(); });

  return { sync, dismiss, isOpen: () => !!state };
}
