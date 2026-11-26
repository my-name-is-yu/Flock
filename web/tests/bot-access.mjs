import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function fixtures() {
  const botId = 'release';
  const connections = Array.from({length: 12}, (_, index) => ({
    id: index + 1,
    botId,
    machineId: 'Laptop',
    threadId: `example-session-${index + 1}`,
    status: index < 2 ? 'approved' : 'pending',
    rules: [{operation: 'read_file', path: '/work/notes.txt'}],
  }));
  window.machineActions = [];
  window.fetch = async (path, options) => {
    if (path === '/api/bots') return Response.json([{id: botId, name: 'Release'}]);
    if (path.startsWith('/api/bot-settings/channels?')) return Response.json({bridges: []});
    if (path.startsWith('/api/bot-settings?')) {
      return Response.json({canManage: true, canManageMembers: false, members: [],
        secretInputs: [{id: 1, name: 'API key', status: 'pending'}]});
    }
    if (path === '/api/channels/local-connections') return Response.json(connections);
    if (path.startsWith('/api/channels/local-connections/') && options.method === 'POST') {
      window.machineActions.push([path.split('/').at(-1), JSON.parse(options.body)]);
      return Response.json({ok: true});
    }
    return new Response(null, {status: 404});
  };
}

async function exercise() {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const secrets = document.getElementById('secrets');
  const input = secrets.querySelector('input');
  const rect = input.getBoundingClientRect();
  check(!secrets.hidden && rect.top >= 0 && rect.bottom <= innerHeight && window.scrollY > 0,
    'The secret link left the asynchronously rendered form offscreen');

  const rows = () => [...document.querySelectorAll('#machines .settings-row')];
  const status = document.getElementById('status');
  const action = async (session, name, success) => {
    const item = rows().find(row => row.querySelector('.row-label small')?.textContent === `T3 thread ${session}`);
    check(item, `Missing the connection for ${session}`);
    const control = [...item.querySelectorAll('button')].find(button => button.textContent === name);
    check(control?.closest('[role="group"]')?.getAttribute('aria-label') === `Laptop · T3 thread ${session}`,
      `Missing a named action for ${session}`);
    control.click();
    for (let attempt = 0; attempt < 20 && status.textContent !== success; attempt++)
      await new Promise(resolve => setTimeout(resolve, 10));
    check(status.textContent === success, `${name} did not complete for ${session}`);
  };
  await action('example-session-4', 'Approve', 'Connection approved.');
  await action('example-session-2', 'Disconnect', 'Computer disconnected.');
  const [approval, disconnection] = window.machineActions;
  check(approval?.[0] === 'decide' && approval[1].id === 4 && approval[1].approve &&
    disconnection?.[0] === 'disconnect' && disconnection[1].id === 2,
    'The controls submitted the wrong connection IDs');
}

const settings = (await readAsset('/bot-settings.html')).replace('<script type="module" src="/bot-settings.js"></script>', `
  <script>(${fixtures})();</script>
  <script type="module">
    import '/bot-settings.js';
    try {
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script>`);
await withAssetServer({ '/settings/release': settings }, async (origin) => {
  requirePass(await dumpDom(process.argv[2], `${origin}/settings/release#secrets`, { flags: ['--window-size=800,540'], budget: 1500 }),
    'The browser did not finish the Bot Access checks');
});
