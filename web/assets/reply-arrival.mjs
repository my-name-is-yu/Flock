const replyKinds = new Set(['conversation_reply', 'completed', 'report', 'waiting']);

export function replyArrivalId(events, previousHead) {
  if (previousHead == null) return;
  const added = events.filter((event) => event.id > previousHead &&
    event.kind !== 'wait_closed' && event.kind !== 'memory_remembered');
  // A prompt and its reply, or a workflow report and its completion, can arrive together.
  if (!added.length || added.length > 2) return;
  const reply = added.at(-1);
  if (!replyKinds.has(reply.kind)) return;
  const first = added[0];
  if (added.length === 1 || !replyKinds.has(first.kind) ||
      (first.kind === 'report' && reply.kind === 'completed' && first.taskId != null && first.taskId === reply.taskId)) {
    return reply.id;
  }
}
