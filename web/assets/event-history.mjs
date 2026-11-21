export async function refreshEventHistory(history, latest, readOlder, isCurrent) {
  const pages = [latest];
  let before = latest.olderBefore;
  while (history.newestEvent != null && before > history.newestEvent) {
    if (!isCurrent()) return;
    const page = await readOlder(before);
    if (page.olderBefore >= before) throw new Error('Invalid conversation cursor');
    pages.push(page);
    before = page.olderBefore;
  }
  if (!isCurrent()) return;

  // Commit only after closing the gap. The user may have loaded older history in the meantime.
  history.olderBefore = history.newestEvent == null ? before : Math.min(history.olderBefore, before);
  for (const page of pages) {
    for (const event of page.events) history.events.set(event.id, event);
  }
  // Zero marks a loaded empty conversation so reconnect can catch up to its beginning.
  history.newestEvent = latest.events.at(-1)?.id ?? history.newestEvent ?? 0;
}
