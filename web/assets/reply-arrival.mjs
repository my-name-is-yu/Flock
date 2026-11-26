import { isMemoryNotice } from './chat-grouping.mjs';

const replyKinds = new Set(['conversation_reply', 'completed', 'report', 'waiting']);

export function replyArrivalId(events, previousHead) {
  if (previousHead == null) return;
  const added = events.filter((event) => event.id > previousHead &&
    event.kind !== 'wait_closed' && !isMemoryNotice(event));
  if (!added.length || added.length > 2) return;
  const reply = added.at(-1);
  if (!replyKinds.has(reply.kind)) return;
  const first = added[0];
  if (added.length === 1 || !replyKinds.has(first.kind)) return reply.id;
  const completesSameTask = first.kind === 'report' && reply.kind === 'completed' &&
    first.taskId != null && first.taskId === reply.taskId;
  if (completesSameTask) return reply.id;
}
