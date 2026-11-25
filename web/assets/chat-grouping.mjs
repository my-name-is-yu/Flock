const groupGapSeconds = 15 * 60;

export function messageSide(event) {
  if (event.kind === 'cancelled' || event.kind === 'memory_remembered' || event.data.sessionWake || event.data.sessionInvitation)
    return 'system';
  if (event.kind === 'bot_message') return 'peer';
  if (event.kind === 'conversation_message' && event.data.channel?.channelKind)
    return event.data.channel.fromViewer === true ? 'user' : 'external';
  return ['conversation_message', 'user_answer'].includes(event.kind) || (event.kind === 'request' && event.data.source !== 'automation') ? 'user' : 'bot';
}

function sameDay(first, second) {
  return new Date(first.time * 1000).toDateString() === new Date(second.time * 1000).toDateString();
}

function nearby(first, second) {
  if (!first || !second) return false;
  const gap = second.time - first.time;
  return gap >= 0 && gap < groupGapSeconds && sameDay(first, second);
}

function timeBreak(previous, event) {
  return !!previous && event.time - previous.time >= groupGapSeconds && sameDay(previous, event);
}

function joinsGroup(first, second, side) {
  return side !== 'system' && side !== 'peer' && nearby(first, second) && first.taskId === second.taskId &&
    messageSide(first) === side && messageSide(second) === side;
}

export function messagePlacement(previous, event, next) {
  const side = messageSide(event);
  return {
    side,
    groupStart: !joinsGroup(previous, event, side),
    groupEnd: !joinsGroup(event, next, side),
    timeBreak: side !== 'system' && timeBreak(previous, event),
    taskStart: previous?.taskId !== event.taskId,
    afterQuestion: previous?.kind === 'waiting' && previous.data.id === event.data.waitId,
  };
}

function joinsPeerGroup(first, second) {
  return nearby(first, second) && first.data.fromBotId === second.data.fromBotId;
}

export function peerMessagePlacement(previous, event, next) {
  return {
    groupStart: !joinsPeerGroup(previous, event),
    groupEnd: !joinsPeerGroup(event, next),
    timeBreak: timeBreak(previous, event),
  };
}
