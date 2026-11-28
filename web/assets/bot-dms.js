import { accentColor, showAvatar } from './avatars.js';
import { botDmUrl } from './bot-dm-url.js';
import { botDmPlacement } from './chat-grouping.mjs';
import { node, setMessageBody } from './dom.js';
import { refreshEventHistory } from './event-history.mjs';
import { createTimelineRows } from './timeline-rows.js';

const params = new URLSearchParams(location.search);
const bot = params.get('bot');
const peer = params.get('peer');
const anchor = params.get('anchor');
const byId = (id) => document.getElementById(id);
const back = byId('dm-back');
const heading = byId('dm-heading');
const faces = byId('dm-faces');
const scroller = byId('dm-scroll');
const status = byId('dm-status');
const older = byId('dm-older');
const peers = byId('dm-peers');
const timeline = byId('dm-timeline');
const latest = byId('dm-latest');
const clock = new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit' });
const day = new Intl.DateTimeFormat(undefined, { dateStyle: 'long' });
const fullTime = new Intl.DateTimeFormat(undefined, { dateStyle: 'long', timeStyle: 'short' });
const updateRows = createTimelineRows(timeline, scroller);
const transcript = { events: new Map(), olderBefore: 0 };
let participants;
let pickerSignature;
let pending = false;

if (bot) back.href = `/?${new URLSearchParams({ bot })}`;
if (peer) {
  if (!anchor) {
    back.href = botDmUrl(bot);
    back.setAttribute('aria-label', 'Back to Bot conversations');
  }
  latest.href = botDmUrl(bot, peer);
  scroller.setAttribute('aria-label', 'Read-only Bot conversation');
}
async function readPage(before) {
  const url = new URL(`/api${botDmUrl(bot ?? '', peer, before ? null : anchor)}`, location.href);
  if (before) url.searchParams.set('before', before);
  const response = await fetch(url);
  if (!response.ok) throw Object.assign(new Error('Could not load Bot conversations'), { status: response.status });
  return response.json();
}

function avatar(participant, className = '') {
  const face = node('span', `avatar ${className}`.trim());
  showAvatar(face, participant);
  return face;
}

function drawHeader(page) {
  const people = page.peer ? [page.bot, page.peer] : [page.bot];
  const label = page.peer ? people.map(({ name }) => name).join(' · ') : `${page.bot.name}’s talks with other Bots`;
  if (JSON.stringify(people) !== JSON.stringify(participants)) {
    participants = people;
    faces.replaceChildren(...people.map(participant => avatar(participant)));
    heading.textContent = label;
    document.title = `${label} · Flock`;
    document.documentElement.style.setProperty('--bot-accent', accentColor(page.bot));
  }
}

function drawPicker(page) {
  const signature = JSON.stringify(page.peers);
  if (pickerSignature !== signature) {
    const focused = document.activeElement?.dataset.peer;
    const rows = page.peers.map(participant => {
      const row = node('li');
      const link = node('a', 'dm-peer');
      link.href = botDmUrl(bot, participant.id);
      link.dataset.peer = participant.id;
      const name = node('span', null, participant.name);
      name.dir = 'auto';
      link.append(avatar(participant), name);
      row.append(link);
      return row;
    });
    peers.replaceChildren(...rows);
    if (focused) rows.find(row => row.firstChild.dataset.peer === focused)?.firstChild.focus();
    pickerSignature = signature;
  }
  peers.hidden = !page.peers.length;
  status.textContent = page.peers.length ? '' : 'No conversations with other Bots yet.';
}

function timestamp(date) {
  const time = node('time', 'msg-time', clock.format(date));
  time.dateTime = date.toISOString();
  time.setAttribute('aria-label', fullTime.format(date));
  return time;
}

function drawMessage(event, sender, place) {
  const side = sender.id === bot ? 'user' : 'bot';
  const row = node('article', `msg event-message ${side}`);
  row.id = `dm-message-${event.id}`;
  row.tabIndex = 0;
  row.classList.toggle('dm-anchored', String(event.id) === anchor);
  row.classList.toggle('group-start', place.groupStart);
  row.classList.toggle('group-continues', !place.groupEnd);
  const meta = node('div', place.groupStart ? 'msg-meta' : 'sr-only');
  meta.append(node('span', 'msg-name', sender.name));
  const line = node('div', 'msg-line');
  const bubble = node('div', 'bubble');
  const text = node('div', 'event-text');
  text.dir = 'auto';
  setMessageBody(text, event.data.textHtml, event.data.text);
  bubble.append(text);
  const actions = node('div', 'msg-actions');
  actions.append(timestamp(new Date(event.time * 1000)));
  line.append(place.groupEnd ? avatar(sender, 'msg-avatar') : node('span', 'msg-avatar-space'), bubble, actions);
  row.append(meta, line);
  return row;
}

function drawTranscript() {
  const events = [...transcript.events.values()].sort((a, b) => a.id - b.id);
  updateRows(transcript, keep => {
    const rows = [];
    let previousDay;
    for (const [index, event] of events.entries()) {
      const date = new Date(event.time * 1000);
      const dateLabel = day.format(date);
      const place = botDmPlacement(events[index - 1], event, events[index + 1]);
      if (dateLabel !== previousDay) {
        rows.push(keep(`day-${event.id}`, dateLabel, () => node('p', 'day', dateLabel)));
        previousDay = dateLabel;
      } else if (place.timeBreak) {
        rows.push(keep(`gap-${event.id}`, event.time, () => {
          const gap = node('p', 'day chat-time-break');
          gap.append(timestamp(date));
          return gap;
        }));
      }
      const sender = participants.find(({ id }) => id === event.data.fromBotId);
      rows.push(keep(event.id.toString(), [event, sender, place], () => drawMessage(event, sender, place)));
    }
    return rows;
  });
  timeline.hidden = !events.length;
  older.hidden = !transcript.olderBefore;
  status.textContent = events.length ? '' : 'No messages yet.';
}

function showError(error) {
  if ([400, 401, 403, 404].includes(error.status)) {
    participants = undefined;
    faces.replaceChildren();
    heading.textContent = 'Bot conversations';
    document.title = 'Bot conversations · Flock';
    transcript.events.clear();
    transcript.newestEvent = undefined;
    transcript.olderBefore = 0;
    updateRows(transcript, () => []);
    peers.replaceChildren();
    pickerSignature = undefined;
    timeline.hidden = peers.hidden = older.hidden = true;
    latest.hidden = !(anchor && error.status === 404);
    status.textContent = 'This Bot conversation isn’t available.';
  } else {
    status.textContent = 'Couldn’t load Bot conversations. Trying again…';
  }
}

async function refresh() {
  if (!bot) {
    status.textContent = 'Choose a Bot in Flock to see its conversations.';
    return;
  }
  if (pending) return;
  pending = true;
  try {
    const page = await readPage();
    drawHeader(page);
    if (!peer) {
      drawPicker(page);
      return;
    }
    const first = transcript.newestEvent == null;
    const atEnd = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 65;
    await refreshEventHistory(transcript, page, readPage, () => true);
    drawTranscript();
    latest.hidden = !page.hasNewer;
    if (first && anchor) {
      const target = byId(`dm-message-${anchor}`);
      target?.scrollIntoView({ block: 'center' });
      target?.focus({ preventScroll: true });
    } else if (!anchor && atEnd) scroller.scrollTop = scroller.scrollHeight;
  } catch (error) {
    showError(error);
  } finally {
    pending = false;
  }
}

older.addEventListener('click', async () => {
  if (pending || !transcript.olderBefore) return;
  pending = true;
  older.disabled = true;
  try {
    const page = await readPage(transcript.olderBefore);
    const top = scroller.scrollTop;
    const height = scroller.scrollHeight;
    for (const event of page.events) transcript.events.set(event.id, event);
    transcript.olderBefore = page.olderBefore;
    drawTranscript();
    scroller.scrollTop = top + scroller.scrollHeight - height;
    if (older.hidden) scroller.focus({ preventScroll: true });
  } catch (error) {
    showError(error);
  } finally {
    pending = false;
    older.disabled = false;
  }
});
timeline.addEventListener('click', event => {
  if (!matchMedia('(hover: none)').matches || !event.target.closest('.bubble') ||
      event.target.closest('a') || window.getSelection()?.toString()) return;
  const row = event.target.closest('article');
  const active = timeline.querySelector('.dm-time-visible');
  active?.classList.remove('dm-time-visible');
  if (active !== row) row.classList.add('dm-time-visible');
});

void refresh();
setInterval(() => { if (!document.hidden) void refresh(); }, 5000);
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) void refresh();
});
window.addEventListener('pageshow', event => {
  if (event.persisted) void refresh();
});
