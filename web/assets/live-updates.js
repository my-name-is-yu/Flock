const pollDelay = 2500;
const streamTimeout = 6000;

export function createLiveUpdates(refresh, changed) {
  let path;
  let stream;
  let polling;
  let timeout;
  let live = false;

  function closeStream() {
    stream?.close();
    stream = null;
    live = false;
    clearTimeout(timeout);
  }

  function stopPolling() {
    clearInterval(polling);
    polling = null;
  }

  function startPolling() {
    polling ??= setInterval(() => {
      refresh();
      connect();
    }, pollDelay);
  }

  function connect() {
    if (!path || stream || typeof EventSource === 'undefined') return;
    let current;
    try { current = new EventSource(path); }
    catch { return; }
    stream = current;
    const lost = () => {
      if (stream !== current) return;
      const wasLive = live;
      closeStream();
      if (wasLive) refresh();
      startPolling();
    };
    const received = () => {
      clearTimeout(timeout);
      timeout = setTimeout(lost, streamTimeout);
    };
    received();
    current.addEventListener('ready', () => {
      if (stream !== current) return;
      received();
      live = true;
      stopPolling();
      refresh();
    });
    current.addEventListener('ping', () => {
      if (stream === current) received();
    });
    current.addEventListener('change', (event) => {
      if (stream !== current) return;
      let update;
      try { update = JSON.parse(event.data); }
      catch { lost(); return; }
      if (!update || typeof update !== 'object') { lost(); return; }
      received();
      changed(update);
    });
    current.addEventListener('error', lost);
  }

  return {
    use(nextPath) {
      if (path === nextPath) return;
      path = nextPath;
      closeStream();
      if (!path) {
        stopPolling();
        return;
      }
      startPolling();
      connect();
    },
    get live() { return live; },
  };
}
