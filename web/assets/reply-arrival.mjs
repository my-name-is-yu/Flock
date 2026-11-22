const replyKinds = new Set(['conversation_reply', 'completed', 'report', 'waiting']);

export function createReplyArrival(show) {
  const seenHeads = new WeakMap();
  return (conversation, events, visible) => {
    const head = conversation?.newestEvent;
    if (head == null) return;
    const previous = seenHeads.get(conversation);
    seenHeads.set(conversation, head);
    if (!visible || previous == null || head <= previous) return;

    const added = events.filter((event) => event.id > previous && event.kind !== 'wait_closed');
    // A reply and its prompt can be delivered together. Larger batches are catch-up.
    if (added.length > 2 || !added.length) return;
    const reply = added.at(-1);
    if (replyKinds.has(reply.kind) && !added.slice(0, -1).some((event) => replyKinds.has(event.kind))) show(reply.id);
  };
}
