export async function collectEventPages(latest, newestEvent, readOlder, isCurrent) {
  const pages = [latest];
  let before = latest.olderBefore;
  while (isCurrent() && newestEvent != null && before > newestEvent) {
    const page = await readOlder(before);
    if (page.olderBefore >= before) throw new Error('Invalid conversation cursor');
    pages.push(page);
    before = page.olderBefore;
  }
  return isCurrent() ? pages : null;
}

export function mergeEventPages(history, pages) {
  if (!history.events.size) history.olderBefore = pages[0].olderBefore;
  else if (pages.length > 1) history.olderBefore = Math.min(history.olderBefore, pages.at(-1).olderBefore);
  for (const page of pages) {
    for (const event of page.events) history.events.set(event.id, event);
  }
  history.newestEvent = pages[0].events.at(-1)?.id ?? history.newestEvent;
}
