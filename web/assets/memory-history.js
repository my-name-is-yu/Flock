import { node } from './dom.js';


const date = new Intl.DateTimeFormat('en', { dateStyle: 'medium', timeStyle: 'short', hourCycle: 'h23' });

export function createMemoryHistory(readPage, openConversationEvent, sessionLink, botName) {
  const card = node('section', 'settings-card memory-history');
  const heading = node('h2', 'settings-card-label', 'Memory history');
  const list = node('ol', 'memory-history-list');
  const status = node('p', 'settings-card-feedback');
  status.setAttribute('role', 'status');
  const more = node('button', 'text-button memory-history-more', 'Show earlier changes');
  more.type = 'button';
  more.hidden = true;
  let before = 0;

  function eventButton(label, eventId) {
    const button = node('button', 'text-button', label);
    button.type = 'button';
    button.addEventListener('click', () => openConversationEvent(eventId));
    return button;
  }

  function revisionRow({ author, source, learning, revertedFrom, createdAt, diffSummary }) {
    const item = node('li');
    const label = source ? 'Learned' : revertedFrom ? 'Undid a change' : 'Edited memory';
    const editor = author.startsWith('bot:') ? botName : author.startsWith('person:') ? author.slice('person:'.length) : author;
    item.append(node('strong', null, label), node('span', 'memory-history-meta', ` · ${editor} · ${date.format(createdAt * 1000)}`));
    if (learning?.added.length) item.append(node('p', null, learning.added.map((fact) => `“${fact}”`).join(' · ')));
    if (learning?.removed.length) item.append(node('p', null, 'Replaced · ' + learning.removed.map((fact) => `“${fact}”`).join(' · ')));
    item.append(node('p', 'memory-history-meta', diffSummary));
    if (!learning) return item;
    const provenance = node('p', 'memory-history-source', 'From ');
    provenance.append(eventButton(learning.taskTitle || 'Source request', learning.requestEventId),
      ' · ', eventButton('Session report', learning.reportEventId));
    const session = sessionLink(learning.run);
    if (session) provenance.append(' · ', session);
    item.append(provenance);
    return item;
  }

  async function load() {
    more.disabled = true;
    status.textContent = 'Loading…';
    try {
      const page = await readPage(before);
      list.append(...page.revisions.map(revisionRow));
      before = page.before;
      more.hidden = !before;
      status.textContent = list.childElementCount ? '' : 'No memory changes yet.';
    } catch {
      status.textContent = 'Couldn’t load memory history.';
      more.hidden = !before;
    } finally {
      more.disabled = false;
    }
  }

  more.addEventListener('click', load);
  card.append(heading, list, status, more);
  void load();
  return card;
}
