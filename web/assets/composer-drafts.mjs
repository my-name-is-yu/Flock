const kinds = new Set(['wait', 'message', 'result']);
const replyTarget = (reply) => kinds.has(reply?.kind) && Number.isSafeInteger(reply.messageId) && reply.messageId > 0;

export function createComposerDraftStore(storage = () => sessionStorage) {
  const conversations = new Map();
  const read = (key) => {
    try { return JSON.parse(storage().getItem(key)); }
    catch { return null; }
  };

  return (botId, person) => {
    const key = `flock-composer:${JSON.stringify([person, botId])}`;
    // An absent identity (for example, after a server downgrade) never reads persisted drafts.
    const cacheKey = person === undefined ? `volatile:${botId}` : key;
    if (conversations.has(cacheKey)) return conversations.get(cacheKey);

    const saved = person === undefined ? null : read(key);
    const texts = new Map(Object.entries(saved?.texts ?? {}));
    const pending = new Map(Object.entries(saved?.pending ?? {}));
    const suggestions = new Set(Array.isArray(saved?.suggestions) ? saved.suggestions : []);
    let reply = replyTarget(saved?.reply) ? saved.reply : null;
    const slot = () => String(reply?.messageId ?? 0);
    function persist() {
      if (person === undefined) return false;
      const selected = reply && texts.has(slot()) ? { kind: reply.kind, messageId: reply.messageId } : null;
      try {
        if (texts.size || pending.size) storage().setItem(key, JSON.stringify({
          texts: Object.fromEntries(texts), pending: Object.fromEntries(pending), suggestions: [...suggestions], reply: selected,
        }));
        else storage().removeItem(key);
        return true;
      } catch { return false; }
    }

    // Earlier versions kept one unscoped pending send per Bot. It can only be
    // attributed to a person on installations using the built-in local owner.
    if (person === null && !saved) {
      const legacyKey = `flock-pending-message:${botId}`;
      const legacy = read(legacyKey);
      const payload = legacy?.payload;
      const anchor = payload?.replyToMessageId;
      if (payload?.botId === botId && typeof payload.text === 'string' && typeof payload.clientMessageId === 'string' &&
          (anchor === undefined || (replyTarget(legacy.reply) && legacy.reply.messageId === anchor))) {
        reply = anchor ? legacy.reply : null;
        texts.set(slot(), payload.text);
        pending.set(slot(), { text: payload.text, clientMessageId: payload.clientMessageId });
        if (persist()) {
          try { storage().removeItem(legacyKey); }
          catch {}
        }
      }
    }

    const textAt = (anchor) => {
      const text = texts.get(String(anchor));
      return typeof text === 'string' ? text : '';
    };
    const conversation = {
      get reply() { return reply; },
      get text() { return textAt(slot()); },
      textAt,
      openReply(target, suggestion) {
        reply = target;
        const anchor = slot();
        if (suggestion && (!textAt(anchor) || suggestions.has(anchor))) {
          texts.set(anchor, suggestion);
          suggestions.add(anchor);
        }
        persist();
      },
      closeReply() {
        reply = null;
        persist();
      },
      edit(text) {
        suggestions.delete(slot());
        if (text) texts.set(slot(), text);
        else texts.delete(slot());
        persist();
      },
      prepareSend() {
        const anchor = slot();
        const text = textAt(anchor);
        const previous = pending.get(anchor);
        const clientMessageId = previous?.text === text && typeof previous.clientMessageId === 'string'
          ? previous.clientMessageId : crypto.randomUUID();
        pending.set(anchor, { text, clientMessageId });
        suggestions.delete(anchor);
        persist();
        const payload = { botId, clientMessageId, text };
        if (reply) payload.replyToMessageId = reply.messageId;
        return payload;
      },
      settleSend(payload, received) {
        const anchor = payload.replyToMessageId ?? 0;
        const key = String(anchor);
        if (pending.get(key)?.clientMessageId !== payload.clientMessageId) return;
        pending.delete(key);
        if (received && textAt(anchor) === payload.text) {
          texts.delete(key);
          if (reply?.messageId === anchor) reply = null;
        }
        persist();
      },
    };
    conversations.set(cacheKey, conversation);
    return conversation;
  };
}
