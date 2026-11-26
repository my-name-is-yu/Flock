import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

async function exercise() {
  const { loadInstallation } = await import('/installation-settings.js');
  const check = (value, message) => { if (!value) throw new Error(message); };
  const installation = (system) => ({ access: { mode: 'local', users: [] }, connections: {}, system });
  let current;
  window.fetch = async (path) => {
    if (path === '/api/settings/installation') return Response.json(installation(current));
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
  async function show(system) {
    current = system;
    await loadInstallation();
    return [...document.querySelectorAll('#system .settings-row')].map(item => ({
      title: item.querySelector('strong').textContent,
      value: item.querySelector('small')?.textContent,
      next: item.querySelector('small b')?.textContent,
      change: !!item.querySelector('button'),
      badge: item.querySelector('.setting-state')?.textContent,
    }));
  }
  const neither = { port: false, sessionRead: false };

  let rows = await show({
    active: { port: 52011, sessionRead: 'off' }, saved: { port: null, sessionRead: null },
    next: { port: null, sessionRead: 'off' }, overrides: neither, pending: neither,
  });
  check(rows.length === 2, 'A fresh start showed a restart notice');
  check(rows[0].value === '52011' && !rows[0].next, `An automatic port with nothing saved read “${rows[0].value}”`);
  check(rows[1].value === 'Off' && rows[0].change && rows[1].change, 'A fresh start did not show its values with Change');

  rows = await show({
    active: { port: 52011, sessionRead: 'off' }, saved: { port: null, sessionRead: 'on_demand' },
    next: { port: null, sessionRead: 'on_demand' }, overrides: neither, pending: { port: false, sessionRead: true },
  });
  check(rows[0].value === '52011', `A pending Read T3 threads change made the port read “${rows[0].value}”`);
  check(rows[1].value === 'Off → When needed after restart' && rows[1].next === 'When needed',
    `A pending Read T3 threads change read “${rows[1].value}”`);
  check(rows[2]?.title === 'Restart required', 'A pending change did not ask for a restart');

  rows = await show({
    active: { port: 5000, sessionRead: 'off' }, saved: { port: null, sessionRead: null },
    next: { port: null, sessionRead: 'off' }, overrides: neither, pending: { port: true, sessionRead: false },
  });
  check(rows[0].value === '5000 → Automatic after restart' && rows[0].next === 'Automatic',
    `Returning to an automatic port read “${rows[0].value}”`);
  check(rows[1].value === 'Off' && rows[2]?.title === 'Restart required', 'A pending port change changed the other row');

  rows = await show({
    active: { port: 52011, sessionRead: 'on_demand' }, saved: { port: 5000, sessionRead: 'prefill' },
    next: { port: null, sessionRead: 'on_demand' }, overrides: { port: true, sessionRead: true }, pending: neither,
  });
  check(rows.length === 2 && rows[0].value === '52011' && rows[1].value === 'When needed',
    `Launch settings read “${rows[0].value}” and “${rows[1].value}”`);
  check(rows.every(item => item.badge === 'Set at launch' && !item.change), 'Launch settings could be changed here');
}

function fixturesUx() {
  window.__settingsUx = { disconnectCalls: [], failRule: false };
  const state = window.__settingsUx;
  const installation = {
    access: { mode: 'local', users: [] },
    connections: {
      channels: {
        hostManaged: false,
        canManageHost: true,
        bridges: [],
        localMachines: [{ id: 'mac', personId: 'owner' }],
      },
      tools: {
        hostManaged: false,
        searchKeySet: false,
        mcpServers: [{ id: 'docs', url: 'https://example.com' }],
        skills: [{ id: 'write', path: 'writing', available: true }],
      },
    },
    system: {
      active: { port: 52011, sessionRead: 'off' },
      saved: { port: null, sessionRead: null },
      next: { port: null, sessionRead: 'off' },
      overrides: { port: false, sessionRead: false },
      pending: { port: false, sessionRead: false },
    },
  };
  window.fetch = async (path, options = {}) => {
    if (path === '/api/settings') {
      return Response.json({
        mode: 'supervised',
        rules: [],
        canManagePolicy: true,
        model: { demo: true },
        personOnlyActions: [],
        t3Setup: { running: false },
        canDisconnectT3: true,
        t3: { environments: [{ environmentId: 'local', authorization: { label: 'Mac Studio' } }] },
      });
    }
    if (path === '/api/settings/installation') return Response.json(installation);
    if (path === '/api/settings/rule' && options.method === 'POST' && state.failRule) {
      return Response.json({ error: 'failed' }, { status: 500 });
    }
    if (path === '/api/settings/t3-disconnect' && options.method === 'POST') {
      state.disconnectCalls.push(JSON.parse(options.body));
      return Response.json({ ok: true });
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exerciseUx() {
  const $ = (id) => document.getElementById(id);
  const q = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 60; attempt++) {
      if (predicate()) return;
      await new Promise((resolve) => setTimeout(resolve, 15));
    }
    check(predicate(), message);
  }
  const btn = (root, text) => [...root.querySelectorAll('button')].find((el) => el.textContent === text);

  await waitFor(() => $('rule-action')?.options.length && q('#connections button') && q('#integrations button') && q('#tools button'),
    'Settings UX fixture did not finish loading');

  // 1. Dynamic #rule-form target placeholder.
  const action = $('rule-action');
  const target = $('rule-form').elements.target;
  action.value = 'repository.pr';
  action.dispatchEvent(new Event('change', { bubbles: true }));
  check(target.placeholder === 'github.com/acme/app', `Unexpected repository placeholder: ${target.placeholder}`);
  action.value = 'web.fetch';
  action.dispatchEvent(new Event('change', { bubbles: true }));
  check(target.placeholder === 'example.com', `Unexpected web placeholder: ${target.placeholder}`);
  action.value = 'mcp.invoke';
  action.dispatchEvent(new Event('change', { bubbles: true }));
  check(target.placeholder === 'server-name', `Unexpected mcp placeholder: ${target.placeholder}`);
  action.value = 'skill.read';
  action.dispatchEvent(new Event('change', { bubbles: true }));
  check(target.placeholder === 'skill-name', `Unexpected skill placeholder: ${target.placeholder}`);
  action.value = 'secret.use';
  action.dispatchEvent(new Event('change', { bubbles: true }));
  check(target.placeholder === 'SECRET_NAME', `Unexpected secret placeholder: ${target.placeholder}`);

  // 2. #status.error styling and clear on tab switch.
  window.__settingsUx.failRule = true;
  target.value = 'MY_SECRET';
  $('rule-form').requestSubmit();
  await waitFor(() => $('status').textContent.includes('Couldn’t save') && $('status').classList.contains('error'),
    'Failed rule save did not mark #status as error');
  location.hash = '#connections';
  window.dispatchEvent(new HashChangeEvent('hashchange'));
  check($('status').textContent === '' && !$('status').classList.contains('error'),
    'Switching settings tabs did not clear #status error');

  // 3. T3 Disconnect confirmation dialog.
  const disconnect = btn($('connections'), 'Disconnect');
  check(disconnect, 'Disconnect button missing');
  disconnect.click();
  await waitFor(() => $('installation-editor').open && $('installation-editor-title').textContent === 'Disconnect Mac Studio?',
    'T3 Disconnect did not open confirmation dialog');
  check(window.__settingsUx.disconnectCalls.length === 0, 'Disconnect ran before confirmation');
  btn($('installation-editor'), 'Disconnect').click();
  await waitFor(() => window.__settingsUx.disconnectCalls.length === 1 && !$('installation-editor').open,
    'Confirming T3 disconnect did not submit or close');
  check(window.__settingsUx.disconnectCalls[0].environmentId === 'local', 'Disconnect submitted wrong environmentId');

  // 4. Cancel edit in editMachines, editMcp, and editSkill.
  const verifyCancelEdit = async (section, manageRowText, submitEditLabel, submitAddLabel) => {
    const row = [...section.querySelectorAll('.settings-row')].find((el) => el.textContent.includes(manageRowText));
    row.querySelector('button').click();
    await waitFor(() => $('installation-editor').open && btn($('installation-editor'), 'Edit'),
      `Could not open editor for ${manageRowText}`);
    btn($('installation-editor'), 'Edit').click();
    await waitFor(() => btn($('installation-editor'), 'Cancel edit') &&
      $('installation-editor').querySelector('input[name="id"]')?.readOnly === true &&
      btn($('installation-editor'), submitEditLabel),
    `Edit mode for ${manageRowText} missed Cancel edit or readonly id`);
    btn($('installation-editor'), 'Cancel edit').click();
    await waitFor(() => !btn($('installation-editor'), 'Cancel edit') &&
      $('installation-editor').querySelector('input[name="id"]')?.readOnly === false &&
      $('installation-editor').querySelector('input[name="id"]')?.value === '' &&
      btn($('installation-editor'), submitAddLabel),
    `Cancel edit for ${manageRowText} did not reset the form`);
    $('installation-editor-close').click();
  };
  await verifyCancelEdit($('integrations'), 'Local machines', 'Save machine', 'Add machine');
  await verifyCancelEdit($('tools'), 'MCP servers', 'Save server', 'Add server');
  await verifyCancelEdit($('tools'), 'Skills', 'Save skill', 'Add skill');
}

const page = (await readAsset('/settings.html')).replace('<script type="module" src="/settings.js"></script>', () => `
  <script type="module">
    try {
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error?.stack || error?.message || String(error);
    }
  </script>`);
const uxPage = (await readAsset('/settings.html')).replace('<script type="module" src="/settings.js"></script>', () => `
  <script>(${fixturesUx})();</script>
  <script type="module">
    import '/settings.js';
    try {
      await (${exerciseUx})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error?.stack || error?.message || String(error);
    }
  </script>`);
await withAssetServer({ '/settings': page, '/settings-ux': uxPage }, async (origin) => {
  for (const path of ['/settings#system', '/settings-ux#permissions']) {
    requirePass(await dumpDom(process.argv[2], `${origin}${path}`), 'The browser did not finish the System settings checks', path);
  }
});
