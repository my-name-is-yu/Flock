import { element } from './settings-ui.js';

const date = new Intl.DateTimeFormat('en', { dateStyle: 'medium', timeStyle: 'short' });

export function createMemoryHistory(readPage, openConversationEvent, sessionLink) {
  const card = element('section', '', 'settings-card memory-history');
  const heading = element('h2', 'Memory history', 'settings-card-label');
  const list = element('ol', '', 'memory-history-list');
  const status = element('p', '', 'settings-card-feedback');
  status.setAttribute('role', 'status');
  const more = element('button', 'Show earlier changes', 'text-button memory-history-more');
  more.type = 'button';
  more.hidden = true;
  let before = 0;

  function eventButton(label, eventId) {
    const button = element('button', label, 'text-button');
    button.type = 'button';
    button.addEventListener('click', () => openConversationEvent(eventId));
    return button;
  }

  function revisionRow({ source, learning, revertedFrom, createdAt, diffSummary }) {
    const item = element('li');
    const label = source ? 'Learned' : revertedFrom ? 'Undid a change' : 'Edited memory';
    item.append(element('strong', label), element('span', ` · ${date.format(createdAt * 1000)}`, 'memory-history-meta'));
    if (learning?.added.length) item.append(element('p', learning.added.map((fact) => `“${fact}”`).join(' · ')));
    if (learning?.removed.length) item.append(element('p', 'Replaced · ' + learning.removed.map((fact) => `“${fact}”`).join(' · ')));
    item.append(element('p', diffSummary, 'memory-history-meta'));
    if (!learning) return item;
    const provenance = element('p', 'From ', 'memory-history-source');
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
      status.textContent = 'Could not load memory history.';
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
