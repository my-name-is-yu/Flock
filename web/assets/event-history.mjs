export async function refreshEventHistory(history, latest, readOlder, isCurrent, reprojectLoaded = false) {
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

  if (reprojectLoaded && history.newestEvent != null) {
    let projectionBefore = latest.olderBefore;
    while (projectionBefore > history.olderBefore) {
      if (!isCurrent()) return;
      projectionBefore = await readPage(projectionBefore, true);
    }
  }

  const older = (latest.changedMessageIds ?? []).filter(id => id < before && history.events.has(id)).sort((a, b) => b - a);
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

function sameRunResult(report, completion) {
  if (report.kind !== 'report' || completion?.kind !== 'completed'
      || !Number.isSafeInteger(report.taskId) || report.taskId !== completion.taskId) return false;
  const recorded = report.data;
  const final = completion.data;
  return recorded.status === 'completed' && recorded.truncated === false
    && typeof recorded.report === 'string' && recorded.report.trim() !== '' && recorded.report === final.text
    && Number.isSafeInteger(recorded.revisionId) && recorded.revisionId === final.revisionId
    && [recorded.run?.environmentId, recorded.run?.threadId, recorded.resultRunId].every(id => typeof id === 'string' && id !== '')
    && recorded.run.environmentId === final.run?.environmentId
    && recorded.run.threadId === final.run?.threadId
    && recorded.resultRunId === final.run?.runId;
}

export function timelineMessages(events, dayOf) {
  const messages = [];
  for (let index = 0; index < events.length; index++) {
    const event = events[index];
    const next = events[index + 1];
    if (sameRunResult(event, next) && dayOf(event.time) === dayOf(next.time)) {
      messages.push({ event: next, report: event });
      index++;
    } else if (event.kind !== 'wait_closed') {
      messages.push({ event });
    }
  }
  return messages;
}
