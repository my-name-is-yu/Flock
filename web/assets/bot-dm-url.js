export function botDmUrl(bot, peer, anchor) {
  const params = new URLSearchParams({ bot });
  if (peer) params.set('peer', peer);
  if (anchor) params.set('anchor', anchor);
  return `/bot-dms?${params}`;
}
