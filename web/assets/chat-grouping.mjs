const groupGapSeconds = 15 * 60;

export function messageSide(event) {
  if (event.kind === 'cancelled') return 'system';
  if (event.kind === 'bot_message') return 'peer';
  if (event.kind === 'conversation_message' && event.data.channel?.channelKind)
    return event.data.channel.fromViewer === true ? 'user' : 'external';
  return ['conversation_message', 'user_answer'].includes(event.kind) || (event.kind === 'request' && event.data.source !== 'automation') ? 'user' : 'bot';
}

function sameDay(first, second) {
  return new Date(first.time * 1000).toDateString() === new Date(second.time * 1000).toDateString();
}

function joinsGroup(first, second, side) {
  if (!first || !second || side === 'system' || side === 'peer') return false;
  const gap = second.time - first.time;
  return gap >= 0 && gap < groupGapSeconds && first.taskId === second.taskId &&
    messageSide(first) === side && messageSide(second) === side && sameDay(first, second);
}

export function messagePlacement(previous, event, next) {
  const side = messageSide(event);
  return {
    side,
    groupStart: !joinsGroup(previous, event, side),
    groupEnd: !joinsGroup(event, next, side),
    timeBreak: !!previous && side !== 'system' && event.time - previous.time >= groupGapSeconds && sameDay(previous, event),
    taskStart: previous?.taskId !== event.taskId,
    afterQuestion: previous?.kind === 'waiting' && previous.data.id === event.data.waitId,
  };
}
