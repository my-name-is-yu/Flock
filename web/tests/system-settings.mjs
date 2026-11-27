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
  const neither = { port: false, remoteAddress: false };

  let rows = await show({
    active: { port: 52011, remoteAddress: null }, saved: { port: null, remoteAddress: null },
    next: { port: null, remoteAddress: null }, overrides: neither, pending: neither,
  });
  check(rows.length === 2, 'A fresh start showed a restart notice');
  check(rows[1].title === 'Address for other computers' && rows[1].value === 'Off' && rows[1].change,
    'A fresh start did not offer an address for other computers');
  check(rows[0].value === '52011' && !rows[0].next && rows[0].change, `An automatic port with nothing saved read “${rows[0].value}”`);

  rows = await show({
    active: { port: 5000, remoteAddress: null }, saved: { port: null, remoteAddress: null },
    next: { port: null, remoteAddress: null }, overrides: neither, pending: { port: true, remoteAddress: false },
  });
  check(rows[0].value === '5000 → Automatic after restart' && rows[0].next === 'Automatic',
    `Returning to an automatic port read “${rows[0].value}”`);
  check(rows[1].value === 'Off' && rows[2]?.title === 'Restart required', 'A pending port change changed the other row');

  rows = await show({
    active: { port: 52011, remoteAddress: null }, saved: { port: 5000, remoteAddress: null },
    next: { port: null, remoteAddress: null }, overrides: { port: true, remoteAddress: false }, pending: neither,
  });
  check(rows.length === 2 && rows[0].value === '52011' && rows[0].badge === 'Set at launch' && !rows[0].change,
    `Launch settings read “${rows[0].value}” or could be changed here`);

  rows = await show({
    active: { port: 52011, remoteAddress: null }, saved: { port: null, remoteAddress: '100.64.0.2' },
    next: { port: null, remoteAddress: '100.64.0.2' }, overrides: neither, pending: { ...neither, remoteAddress: true },
  });
  check(rows[1].value === 'Off → 100.64.0.2 after restart' && rows[2]?.title === 'Restart required',
    `A pending address for other computers read “${rows[1].value}”`);
}

function fixturesUx() {
  window.__settingsUx = { disconnectCalls: [], remoteCalls: [], remoteMachine: null, failRule: false };
  const state = window.__settingsUx;
  const remoteUrl = 'http://100.64.0.2:4173/mcp/flock';
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
      active: { port: 52011, remoteAddress: null },
      saved: { port: null, remoteAddress: null },
      next: { port: null, remoteAddress: null },
      overrides: { port: false, remoteAddress: false },
      pending: { port: false, remoteAddress: false },
    },
  };
  window.fetch = async (path, options = {}) => {
    if (path === '/api/settings') {
      const connected = !state.disconnectCalls.length;
      return Response.json({
        mode: 'supervised',
        rules: [],
        canManagePolicy: true,
        model: { demo: true },
        personOnlyActions: [],
        t3Setup: { running: false },
        canDisconnectT3: true,
        t3: connected ? { environments: [{ environmentId: 'local', authorization: { label: 'Mac Studio' } }] } : null,
        remoteMachines: connected || state.remoteMachine
          ? { url: remoteUrl, machines: state.remoteMachine ? [state.remoteMachine] : [] } : null,
      });
    }
    if (path === '/api/settings/installation') return Response.json(installation);
    if (path === '/api/settings/rule' && options.method === 'POST' && state.failRule) {
      return Response.json({ error: 'failed' }, { status: 500 });
    }
    if (path === '/api/settings/remote-machine' && options.method === 'POST') {
      const request = JSON.parse(options.body);
      state.remoteCalls.push(request);
      if (state.remoteMachine && request.remove === state.remoteMachine.id) {
        state.remoteMachine = null;
        return Response.json({ ok: true });
      }
      if (request.name) {
        state.remoteMachine = { id: 'mini', name: request.name };
        return Response.json({ id: 'mini', url: remoteUrl, bearerToken: 'flock-t3-remote' }, { status: 201 });
      }
      return Response.json({ error: 'not_found' }, { status: 404 });
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

  const remoteSection = $('remote-machines-section');
  const remoteMachines = $('remote-machines');
  const editor = $('installation-editor');
  const editorTitle = $('installation-editor-title');
  const addMachine = btn(remoteMachines, 'Add computer');
  check(!remoteSection.hidden && addMachine, 'Other computers did not offer Add computer');
  addMachine.click();
  await waitFor(() => editor.open && editor.querySelector('input[name="name"]'), 'Add computer did not ask for a name');
  editor.querySelector('input[name="name"]').value = ' Mac mini ';
  editor.querySelector('form').requestSubmit();
  await waitFor(() => editorTitle.textContent === 'Set up Mac mini', 'Adding a machine did not show its setup');
  check(window.__settingsUx.remoteCalls[0].name === 'Mac mini', 'Add machine submitted the wrong name');
  const setup = editor.querySelector('.installation-description').textContent;
  check(setup.includes('If you set up Flock in Claude Code before, first run claude mcp remove flock --scope user'),
    'Setup did not explain how to replace the existing Claude Code registration');
  check(setup.includes('In ~/.codex/config.toml, replace the existing [mcp_servers.flock] block or add it if it’s missing'),
    'Setup did not explain how to avoid duplicate Codex tables');
  const [claude, codex] = [...editor.querySelectorAll('textarea')].map((field) => field.value);
  check(claude.startsWith('claude mcp add-json --scope user flock ') && claude.includes('"Authorization":"Bearer flock-t3-remote"') &&
    claude.includes('http://100.64.0.2:4173/mcp/flock'), `Wrong Claude Code setup: ${claude}`);
  check(codex.includes('[mcp_servers.flock]') && codex.includes('Authorization = "Bearer flock-t3-remote"'), `Wrong Codex setup: ${codex}`);
  await waitFor(() => remoteMachines.textContent.includes('Mac mini'), 'The new machine was not listed');
  btn(editor, 'Done').click();
  check(!editor.open, 'Done did not dismiss the one-time setup');

  const disconnect = btn($('connections'), 'Disconnect');
  check(disconnect, 'Disconnect button missing');
  disconnect.click();
  await waitFor(() => editor.open && editorTitle.textContent === 'Disconnect Mac Studio?',
    'T3 Disconnect did not open confirmation dialog');
  check(window.__settingsUx.disconnectCalls.length === 0, 'Disconnect ran before confirmation');
  btn(editor, 'Disconnect').click();
  await waitFor(() => window.__settingsUx.disconnectCalls.length === 1 && !editor.open,
    'Confirming T3 disconnect did not submit or close');
  check(window.__settingsUx.disconnectCalls[0].environmentId === 'local', 'Disconnect submitted wrong environmentId');

  const removeMachine = btn(remoteMachines, 'Remove');
  check(!remoteSection.hidden && remoteMachines.textContent.includes('Mac mini') && removeMachine && !btn(remoteMachines, 'Add computer'),
    'Disconnecting T3 did not leave the machine available for removal only');
  removeMachine.click();
  await waitFor(() => editor.open && editorTitle.textContent === 'Remove Mac mini?', 'Removing the machine did not ask for confirmation');
  btn(editor, 'Remove').click();
  await waitFor(() => !editor.open && remoteSection.hidden, 'Removing the last machine after disconnect did not hide the machine section');
  check(window.__settingsUx.remoteCalls[1].remove === 'mini', 'Remove submitted the wrong machine id');

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
  await verifyCancelEdit($('integrations'), 'Local computers', 'Save computer', 'Add computer');
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
