export async function refreshEventHistory(history, latest, readOlder, isCurrent) {
  const updates = new Map(latest.events.map(event => [event.id, event]));
  async function readPage(cursor, loadedOnly) {
    const page = await readOlder(cursor);
    if (page.olderBefore >= cursor) throw new Error('Invalid conversation cursor');
    for (const event of page.events) {
      if (!loadedOnly || history.events.has(event.id)) updates.set(event.id, event);
    }
    return page.olderBefore;
  }

  let before = latest.olderBefore;
  while (history.newestEvent != null && before > history.newestEvent) {
    if (!isCurrent()) return;
    before = await readPage(before, false);
  }

  const changed = new Set(latest.changedMessageIds);
  for (const event of history.events.values()) {
    if (event.kind === 'conversation_reply' && event.data.route === 'model_budget' && event.data.status === 'needs_you') {
      changed.add(event.id);
      changed.add(event.data.replyToMessageId);
    }
  }
  const older = [...changed].filter(id => id < before && history.events.has(id)).sort((a, b) => b - a);
  let uncovered = before;
  for (const id of older) {
    if (!isCurrent()) return;
    if (id < uncovered) uncovered = await readPage(id + 1, true);
  }
  if (!isCurrent()) return;

  history.olderBefore = history.newestEvent == null ? before : Math.min(history.olderBefore, before);
  for (const [id, event] of updates) history.events.set(id, event);
  history.newestEvent = latest.events.at(-1)?.id ?? history.newestEvent ?? 0;
}
