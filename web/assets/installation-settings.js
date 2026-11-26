import { actionButton, element, row, rowLabel } from '/settings-ui.js';

const $ = (id) => document.getElementById(id);
const editor = $('installation-editor');
const feedback = $('installation-editor-status');
let installation;
let saving = false;
let returnFocus;

export function postSettings(path, data) {
  return fetch('/api/settings/' + path, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Flock-Action': '1'},
    body: JSON.stringify(data),
  });
}

function showFeedback(message) {
  feedback.textContent = message;
  feedback.hidden = !message;
}

function openEditor(title, description, ...content) {
  if (!editor.open) returnFocus = document.activeElement;
  $('installation-editor-title').textContent = title;
  $('installation-editor-content').replaceChildren(
    element('p', description, 'installation-description'), ...content,
  );
  showFeedback('');
  if (!editor.open) editor.showModal();
  const firstField = $('installation-editor-content').querySelector('input:not([readonly]), select');
  (firstField ?? $('installation-editor-close')).focus({preventScroll: true});
}

$('installation-editor-close').onclick = () => { if (!saving) editor.close(); };
editor.addEventListener('cancel', (event) => { if (saving) event.preventDefault(); });
editor.addEventListener('close', () => {
  $('installation-editor-content').replaceChildren();
  showFeedback('');
  const target = returnFocus?.isConnected && returnFocus !== document.body
    ? returnFocus : document.querySelector('[aria-current="page"]');
  if (target) target.focus({preventScroll: true});
  returnFocus = null;
});

function formActions() {
  return element('div', '', 'installation-form-actions span-form');
}

async function runEditorAction(commit) {
  if (saving) return;
  saving = true;
  const controls = [...editor.querySelectorAll('button, input, select')].filter((field) => !field.disabled);
  for (const field of controls) field.disabled = true;
  showFeedback('');
  try {
    if (await commit() !== false) editor.close();
  } catch (error) {
    showFeedback(error.message || 'Could not save this change.');
  } finally {
    saving = false;
    for (const field of controls) field.disabled = false;
  }
}

export function confirmSetting(title, description, label, commit) {
  const actions = formActions();
  actionButton(actions, 'Cancel', () => editor.close());
  const button = actionButton(actions, label, () => void runEditorAction(commit));
  button.classList.add('danger');
  openEditor(title, description, actions);
}

function settingField(name, label, options = {}) {
  const wrapper = element('label', label, options.wide ? 'span-form' : '');
  const input = element(options.choices ? 'select' : 'input');
  input.name = name;
  if (options.choices) {
    for (const [value, text] of Object.entries(options.choices)) {
      const option = element('option', text);
      option.value = value;
      input.append(option);
    }
  } else {
    input.type = options.type ?? 'text';
    input.autocomplete = options.type === 'password' ? 'new-password' : 'off';
    input.spellcheck = false;
    if (options.placeholder) input.placeholder = options.placeholder;
    if (input.type === 'number') {
      input.min = '1';
      input.max = '65535';
      input.step = '1';
    }
  }
  input.required = options.required === true;
  input.value = options.value ?? '';
  wrapper.append(input);
  return wrapper;
}

function buildForm(fields, label, commit, actions = []) {
  const form = element('form', '', 'installation-form');
  const buttons = formActions();
  const save = element('button', label, 'button primary');
  save.type = 'submit';
  buttons.append(...actions, save);
  form.append(...fields, buttons);
  form.onsubmit = (event) => {
    event.preventDefault();
    const data = Object.fromEntries(new FormData(form));
    void runEditorAction(() => commit(data));
  };
  return form;
}

async function saveInstallation(section, payload, note = 'Saved.') {
  const response = await postSettings('installation/' + section, payload);
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.message || 'Could not save this change. Check your input and try again.');
  if (note) $('status').textContent = note;
  try { await loadInstallation(); }
  catch { $('status').textContent = 'Saved. Reload to see the current settings.'; }
  return data;
}

export function statusBadge(text, state) {
  const badge = element('span', text, 'setting-state');
  if (state) badge.dataset.state = state;
  return badge;
}

function appendConnectionStates(container, {
  configured, pending, checked, checkedAt, checkedLabel, failedLabel = 'Check failed', hostManaged,
}) {
  if (typeof checked === 'boolean') {
    const badge = statusBadge(checked ? checkedLabel : failedLabel, checked ? 'checked' : 'error');
    if (checkedAt) badge.setAttribute('aria-description', `Last checked ${new Date(checkedAt * 1000).toLocaleString()}`);
    container.append(badge);
  } else if (configured && !hostManaged) container.append(statusBadge('Registered'));
  if (hostManaged) container.append(statusBadge('Managed on host'));
  if (pending) container.append(statusBadge('Restart required', 'pending'));
}

function connectionRow(group, title, description, {manage, readOnly, ...status}) {
  const item = row(group, rowLabel(title, description));
  const actions = element('div', '', 'connection-actions');
  appendConnectionStates(actions, status);
  if (manage) {
    const label = readOnly ? 'View' : status.configured ? 'Manage' : 'Set up';
    actionButton(actions, label, manage).setAttribute('aria-label', `${label} ${title}`);
  }
  if (actions.childElementCount) item.append(actions);
}

function restartNotice(group) {
  const item = row(group, rowLabel('Restart required', 'Saved changes will take effect when Flock next starts.'));
  item.classList.add('restart-notice');
}

function entriesList(entries, description, {edit, remove, test, status} = {}) {
  const group = element('div', '', 'settings-group');
  for (const entry of entries) {
    const item = row(group, rowLabel(entry.id, description(entry)));
    const actions = element('div', '', 'row-actions');
    const states = element('span', '', 'connection-states');
    const renderStates = () => {
      states.replaceChildren();
      if (status) appendConnectionStates(states, status(entry));
    };
    if (status) {
      renderStates();
      actions.append(states);
    }
    if (test) actionButton(actions, 'Check', async (event) => {
      if (await test(entry, event.currentTarget)) renderStates();
    }).setAttribute('aria-label', `Check ${entry.id}`);
    if (edit) actionButton(actions, 'Edit', () => edit(entry)).setAttribute('aria-label', `Edit ${entry.id}`);
    if (remove) {
      const control = actionButton(actions, 'Remove', () => remove(entry));
      control.classList.add('quiet-danger');
      control.setAttribute('aria-label', `Remove ${entry.id}`);
    }
    if (actions.childElementCount) item.append(actions);
  }
  if (!entries.length) group.hidden = true;
  return group;
}

function removeButton(label, action) {
  const control = element('button', label, 'button quiet-danger');
  control.type = 'button';
  control.onclick = action;
  return control;
}

function connectionResult() {
  const note = element('p', '', 'connection-test-result');
  note.setAttribute('role', 'status');
  note.hidden = true;
  return note;
}

async function checkConnection(control, note, section, data) {
  control.disabled = true;
  note.hidden = false;
  note.dataset.result = 'pending';
  note.textContent = 'Checking connection…';
  try {
    const response = await postSettings('installation/' + section, data);
    const {check, message} = await response.json();
    if (!response.ok) throw new Error(message);
    note.dataset.result = check.reachable ? 'ready' : 'error';
    note.textContent = check.message;
    try { await loadInstallation(); } catch {}
    return check;
  } catch (error) {
    note.dataset.result = 'error';
    note.textContent = error.message || 'Could not check the connection. Try again.';
  } finally {
    control.disabled = false;
  }
}

function editGitHub() {
  const {tokenSet, webhookSecretSet} = installation.connections.github;
  const actions = [];
  if (tokenSet) actions.push(removeButton('Remove token', () => confirmSetting(
    'Remove GitHub token?', 'Flock will stop using this credential after restart.', 'Remove token',
    () => saveInstallation('github', {token: null}),
  )));
  if (webhookSecretSet) actions.push(removeButton('Remove webhook secret', () => confirmSetting(
    'Remove webhook secret?', 'Flock will stop accepting signed GitHub notifications after restart.', 'Remove secret',
    () => saveInstallation('github', {webhookSecret: null}),
  )));
  const checkResult = connectionResult();
  if (tokenSet) {
    const check = element('button', 'Check connection', 'button');
    check.type = 'button';
    check.onclick = () => void checkConnection(check, checkResult, 'github', {action: 'check'});
    actions.push(check);
  }
  const form = buildForm([
    settingField('token', 'Access token', {type: 'password', placeholder: tokenSet ? 'Token saved' : '', wide: true}),
    settingField('webhookSecret', 'Webhook secret', {type: 'password', placeholder: webhookSecretSet ? 'Secret saved' : '', wide: true}),
  ], 'Save', ({token, webhookSecret}) => {
    if (!token.trim() && !webhookSecret.trim()) throw new Error('Enter a token or a webhook secret to save.');
    const changes = {};
    if (token.trim()) changes.token = token.trim();
    if (webhookSecret.trim()) changes.webhookSecret = webhookSecret.trim();
    return saveInstallation('github', changes);
  }, actions);
  openEditor('GitHub', 'Register a token for repository access and an optional secret for signed webhook notifications.', form, checkResult);
}

const channelNames = {slack: 'Slack', imessage: 'iMessage'};

function bridgeStatus(bridge) {
  return {
    configured: true, pending: bridge.pending ?? !bridge.active, checked: bridge.reachable, checkedAt: bridge.checkedAt,
    checkedLabel: 'Bridge reachable', failedLabel: 'Bridge unreachable',
  };
}

function editChannel(type, entry = {}) {
  const title = channelNames[type];
  const editing = Boolean(entry.id);
  const channels = installation.connections.channels;
  const entries = channels.bridges.filter((bridge) => bridge.type === type);
  const checkResult = connectionResult();
  const test = async (bridge, control) => {
    const result = await checkConnection(control, checkResult, 'channels', {
      action: 'checkBridge', type, id: bridge.id, port: bridge.port,
    });
    if (result) {
      bridge.reachable = result.reachable;
      bridge.checkedAt = result.checkedAt;
    }
    return result;
  };
  if (channels.hostManaged) {
    openEditor(title, 'The Flock host manages these bridge settings.',
      entriesList(entries, (bridge) => `Local port ${bridge.port}`,
        {test, status: (bridge) => ({...bridgeStatus(bridge), hostManaged: true})}), checkResult);
    return;
  }
  const list = entriesList(entries, (bridge) => `Local port ${bridge.port}`, {
    edit: (bridge) => editChannel(type, bridge),
    remove: (bridge) => confirmSetting(`Remove ${bridge.id}?`,
      'Flock will stop using this bridge after restart.', 'Remove bridge',
      () => saveInstallation('channels', {action: 'removeBridge', type, id: bridge.id})),
    test,
    status: bridgeStatus,
  });
  const fields = [
    settingField('id', type === 'slack' ? 'Workspace ID' : 'Connection name', {value: entry.id, required: true}),
    settingField('port', 'Local bridge port', {type: 'number', value: entry.port, required: true}),
    settingField('credential', 'Bridge secret', {
      type: 'password', placeholder: entry.credentialSet ? 'Secret saved' : '', required: !entry.credentialSet, wide: true,
    }),
  ];
  if (editing) fields[0].querySelector('input').readOnly = true;
  const actions = [];
  if (editing) {
    const cancel = element('button', 'Cancel edit', 'button');
    cancel.type = 'button';
    cancel.onclick = () => editChannel(type);
    actions.push(cancel);
  }
  const form = buildForm(fields, editing ? 'Save bridge' : 'Add bridge', ({id, port, credential}) => {
    const body = {action: 'saveBridge', type, id: id.trim(), port: Number(port)};
    if (credential) body.credential = credential;
    return saveInstallation('channels', body);
  }, actions);
  openEditor(title, 'Register a bridge running on the Flock host. Choose which Bot replies from its Access page.',
    list, checkResult, form);
}

function editMachines(entry = {}) {
  const channels = installation.connections.channels;
  const ownerLabel = (machine) => `Owner · ${machine.personId}`;
  if (channels.hostManaged || !channels.canManageHost) {
    openEditor('Local machines', 'The installation owner manages which hosts can receive requests to read local files.',
      entriesList(channels.localMachines, ownerLabel));
    return;
  }
  const editing = Boolean(entry.id);
  const list = entriesList(channels.localMachines, ownerLabel, {
    edit: editMachines,
    remove: (machine) => confirmSetting(`Remove ${machine.id}?`,
      'New requests will no longer be able to use this machine after restart.',
      'Remove machine', () => saveInstallation('channels', {action: 'removeMachine', id: machine.id})),
    status: registeredEntry,
  });
  const name = settingField('id', 'Machine name', {required: true, value: entry.id});
  if (editing) name.querySelector('input').readOnly = true;
  const form = buildForm([
    name,
    settingField('personId', 'Owner’s person ID', {required: true, value: entry.personId, placeholder: 'local-owner'}),
  ], editing ? 'Save machine' : 'Add machine', ({id, personId}) =>
    saveInstallation('channels', {action: 'saveMachine', id: id.trim(), personId: personId.trim()}));
  openEditor('Local machines', 'Register the Flock host for approved requests to read local files.', list, form);
}

function editChannelOptions() {
  const {artifactBaseUrl, sessionCredentialSet, canManageHost} = installation.connections.channels;
  const actions = [];
  if (canManageHost && sessionCredentialSet) actions.push(removeButton('Remove bridge secret', () => confirmSetting(
    'Remove Session bridge secret?', 'Session bridge requests will stop being accepted after restart.', 'Remove secret',
    () => saveInstallation('channels', {action: 'options', sessionCredential: null}),
  )));
  const fields = [settingField('artifactBaseUrl', 'Shared preview URL', {type: 'url', value: artifactBaseUrl, wide: true})];
  if (canManageHost) fields.push(settingField('sessionCredential', 'Session bridge secret', {
      type: 'password', placeholder: sessionCredentialSet ? 'Secret saved' : '', wide: true,
    }));
  const form = buildForm(fields, 'Save', ({artifactBaseUrl: url, sessionCredential}) => {
    const body = {action: 'options', artifactBaseUrl: url.trim()};
    if (sessionCredential) body.sessionCredential = sessionCredential;
    return saveInstallation('channels', body);
  }, actions);
  openEditor('Bridge options', canManageHost
    ? 'Configure the URL used for channel previews and the secret used by the local Session bridge.'
    : 'Configure the URL used for channel previews. The installation owner manages the Session bridge secret.', form);
}

function editSearch() {
  const actions = [];
  if (installation.connections.tools.searchKeySet) actions.push(removeButton('Remove key', () => confirmSetting(
    'Remove Brave Search key?', 'Flock will stop using this key after restart.', 'Remove key',
    () => saveInstallation('tools', {action: 'search', key: null}),
  )));
  const form = buildForm([
    settingField('key', 'Brave Search API key', {type: 'password', required: true, wide: true}),
  ], 'Save key', ({key}) => saveInstallation('tools', {action: 'search', key: key.trim()}), actions);
  openEditor('Web search', 'Register a Brave Search key for Bots with permission to search the web.', form);
}

function registeredEntry(entry) {
  return {configured: true, pending: entry.pending};
}

function editMcp(entry = {}) {
  const {hostManaged, mcpServers} = installation.connections.tools;
  if (hostManaged) {
    openEditor('MCP servers', 'The Flock host manages these shared connections.', entriesList(mcpServers, (server) => server.url));
    return;
  }
  const editing = Boolean(entry.id);
  const list = entriesList(mcpServers, (server) => server.url, {
    edit: editMcp,
    remove: (server) => confirmSetting(`Remove ${server.id}?`, 'This connection will be removed after restart.', 'Remove server',
      () => saveInstallation('tools', {action: 'removeMcp', id: server.id})),
    status: registeredEntry,
  });
  const name = settingField('id', 'Connection name', {required: true, value: entry.id});
  if (editing) name.querySelector('input').readOnly = true;
  const actions = [];
  if (entry.tokenSet) actions.push(removeButton('Remove token', () => confirmSetting(
    'Remove access token?', `Flock will connect to ${entry.id} without this token after restart.`, 'Remove token',
    () => saveInstallation('tools', {action: 'saveMcp', id: entry.id, url: entry.url, token: null}),
  )));
  const form = buildForm([
    name,
    settingField('url', 'Server URL', {type: 'url', required: true, value: entry.url}),
    settingField('token', 'Access token', {type: 'password', placeholder: entry.tokenSet ? 'Token saved' : 'Optional', wide: true}),
  ], editing ? 'Save server' : 'Add server', ({id, url, token}) => {
    const body = {action: 'saveMcp', id: id.trim(), url: url.trim()};
    if (token) body.token = token;
    return saveInstallation('tools', body);
  }, actions);
  openEditor('MCP servers', 'Register shared tool servers. Flock’s permission rules control which operations Bots can use.', list, form);
}

function editSkill(entry = {}) {
  const {hostManaged, skills} = installation.connections.tools;
  const skillLabel = (skill) => `${skill.path}${skill.available === false ? ' · Folder unavailable' : ''}`;
  if (hostManaged) {
    openEditor('Skills', 'The Flock host manages these skill folders.', entriesList(skills, skillLabel));
    return;
  }
  const editing = Boolean(entry.id);
  const list = entriesList(skills, (skill) => skill.path, {
    edit: editSkill,
    remove: (skill) => confirmSetting(`Remove ${skill.id}?`, 'This skill will leave the shared catalog after restart.', 'Remove skill',
      () => saveInstallation('tools', {action: 'removeSkill', id: skill.id})),
    status: (skill) => skill.available === false
      ? {checked: false, failedLabel: 'Folder unavailable'} : registeredEntry(skill),
  });
  const name = settingField('id', 'Skill name', {required: true, value: entry.id});
  if (editing) name.querySelector('input').readOnly = true;
  const form = buildForm([
    name,
    settingField('path', 'Skill folder', {required: true, value: entry.path, placeholder: 'writing'}),
  ], editing ? 'Save skill' : 'Add skill', ({id, path}) =>
    saveInstallation('tools', {action: 'saveSkill', id: id.trim(), path: path.trim()}));
  openEditor('Skills', 'Register an existing skill folder from the Flock host’s managed skills directory.', list, form);
}

function renderIntegrations(connections) {
  if (!connections) return;
  const {github, channels, tools} = connections;
  $('integrations-section').hidden = false;
  $('tools-section').hidden = false;
  $('integrations').replaceChildren();
  $('tools').replaceChildren();
  if (github) {
    connectionRow('integrations', 'GitHub', 'Repository access and pull request notifications.', {
      configured: github.tokenSet || github.webhookSecretSet, pending: github.pending,
      checked: github.verified, checkedAt: github.checkedAt, checkedLabel: 'Token verified', manage: editGitHub,
    });
  }
  if (channels) {
    const {hostManaged, canManageHost} = channels;
    for (const [type, name] of Object.entries(channelNames)) {
      const bridges = channels.bridges.filter((entry) => entry.type === type);
      const configured = bridges.length > 0;
      const checked = bridges.some((bridge) => bridge.reachable === false) ? false
        : configured && bridges.every((bridge) => bridge.reachable === true) ? true : undefined;
      const description = type === 'slack' ? 'Connect a workspace through its local bridge.'
        : 'Connect Messages through a bridge on the Flock host.';
      connectionRow('integrations', name, description, {
        configured, checked, checkedLabel: bridges.length > 1 ? 'Bridges reachable' : 'Bridge reachable',
        checkedAt: bridges.length === 1 ? bridges[0].checkedAt : undefined,
        failedLabel: 'Bridge check failed', hostManaged, readOnly: hostManaged,
        pending: type === 'slack' ? channels.slackPending : channels.imessagePending,
        manage: !hostManaged || configured ? () => editChannel(type) : null,
      });
    }
    const viewMachines = !hostManaged && canManageHost || channels.localMachines.length > 0;
    connectionRow('integrations', 'Local machines', canManageHost
      ? 'Manage hosts that can receive requests for local files.'
      : 'The installation owner manages access to local machines.', {
      configured: channels.localMachines.length > 0, pending: channels.localMachinesPending,
      hostManaged, readOnly: hostManaged || !canManageHost, manage: viewMachines ? editMachines : null,
    });
    connectionRow('integrations', 'Bridge options', 'Shared preview URL and Session bridge credentials.', {
      configured: Boolean(channels.artifactBaseUrl || channels.sessionCredentialSet), pending: channels.optionsPending,
      hostManaged, manage: hostManaged ? null : editChannelOptions,
    });
  }
  if (tools) {
    const {hostManaged} = tools;
    connectionRow('tools', 'Web search', 'Brave Search for Bots on this Flock.', {
      configured: tools.searchKeySet, pending: tools.searchPending, hostManaged, manage: hostManaged ? null : editSearch,
    });
    connectionRow('tools', 'MCP servers', 'Shared connections to external tools.', {
      configured: tools.mcpServers.length > 0, pending: tools.mcpPending, hostManaged, readOnly: hostManaged,
      manage: !hostManaged || tools.mcpServers.length ? editMcp : null,
    });
    connectionRow('tools', 'Skills', 'Reusable instructions available on the Flock host.', {
      configured: tools.skills.length > 0, pending: tools.skillsPending, hostManaged, readOnly: hostManaged,
      manage: !hostManaged || tools.skills.length ? editSkill : null,
    });
  }
}

function confirmCredential(title, person, submitLabel, commit) {
  const form = buildForm([
    settingField('password', 'Generated password', {type: 'password', required: true, wide: true}),
  ], submitLabel, ({password}) => commit(password));
  openEditor(title, `Enter the generated password for ${person} to confirm you can use it.`, form);
}

function showCredential(title, {person, password}, {activate} = {}) {
  const user = element('p', `Username: ${person}`, 'installation-description');
  const credential = settingField('generated', 'Password', {value: password});
  credential.classList.add('invite-credential');
  const input = credential.querySelector('input');
  input.readOnly = true;
  const actions = formActions();
  const copy = actionButton(actions, 'Copy password', async () => {
    try {
      await navigator.clipboard.writeText(password);
      copy.textContent = 'Copied';
    } catch {
      input.focus();
      input.select();
    }
  });
  if (activate) {
    actionButton(actions, 'Continue', () => confirmCredential(title, person, activate.label, activate.commit), true);
  } else actionButton(actions, 'Done', () => editor.close(), true);
  const description = activate
    ? 'Save this password. You’ll need to enter it in the next step.'
    : 'Share this password with the invited person. It will appear here only once.';
  openEditor(title, description, user, credential, actions);
}

async function activateAdmin(password) {
  const response = await postSettings('installation/access', {operation: 'activate', person: 'local-owner', password});
  if (!response.ok) throw new Error('Could not enable shared access. Generate a new password and try again.');
  location.reload();
  return false;
}

async function prepareAccess(event) {
  const control = event.currentTarget;
  control.disabled = true;
  $('status').textContent = '';
  try {
    const data = await saveInstallation('access', {operation: 'prepare'}, '');
    showCredential('Administrator password', data.credential, {
      activate: {label: 'Enable shared access', commit: activateAdmin},
    });
  } catch (error) {
    $('status').textContent = error.message;
  } finally {
    control.disabled = false;
  }
}

function invitePerson() {
  const form = buildForm([
    settingField('person', 'Username', {required: true, wide: true}),
    settingField('role', 'Role', {value: 'member', choices: {member: 'Member', admin: 'Administrator'}, wide: true}),
  ], 'Create sign-in', async ({person, role}) => {
    const data = await saveInstallation('access', {operation: 'invite', person: person.trim(), adminRoot: role === 'admin'});
    showCredential('Invitation details', data.credential);
    return false;
  });
  openEditor('Invite someone', 'Create a username and a password for this Flock.', form);
}

function changeRole({person, adminRoot}) {
  const form = buildForm([
    settingField('role', 'Role', {value: adminRoot ? 'admin' : 'member', choices: {member: 'Member', admin: 'Administrator'}, wide: true}),
  ], 'Save role', ({role}) => saveInstallation('access', {operation: 'role', person, adminRoot: role === 'admin'}));
  openEditor(`Role for ${person}`, 'Administrators can manage this installation. Members can use Bots shared with them.', form);
}

function resetPassword(person) {
  confirmSetting(`Reset password for ${person}?`, 'The old password will work until you activate the replacement.', 'Generate password',
    async () => {
      const data = await saveInstallation('access', {operation: 'rotate', person}, '');
      showCredential('New password', data.credential, {
        activate: {
          label: 'Activate new password',
          commit: (password) => activatePassword(person, password),
        },
      });
      return false;
    });
}

async function activatePassword(person, password) {
  const response = await postSettings('installation/access', {operation: 'activate', person, password});
  if (!response.ok) throw new Error('Could not activate the new password. Check it and try again.');
  location.reload();
  return false;
}

function renderAccess({mode, users = [], currentPerson, pendingUntil} = {}) {
  $('access').replaceChildren();
  if (mode === 'local') {
    const item = row('access', rowLabel('Private to this computer', 'Enable shared access to create sign-ins for other people.'));
    if (pendingUntil) {
      const actions = element('div', '', 'row-actions');
      actionButton(actions, 'Generate new password', prepareAccess);
      actionButton(actions, 'Confirm password',
        () => confirmCredential('Administrator password', 'local-owner', 'Enable shared access', activateAdmin), true);
      item.append(actions);
    } else actionButton(item, 'Enable sharing', prepareAccess, true);
    return;
  }
  if (mode === 'external') row('access', rowLabel('Managed on the host', 'The host supplies the login credentials for this Flock.'));
  else if (mode === 'managed') {
    const item = row('access', rowLabel('Shared access', 'Sign-in is required to use this Flock.'));
    actionButton(item, 'Invite someone', invitePerson, true);
  } else {
    row('access', rowLabel('Access settings unavailable', 'Sign in as an installation administrator to manage access.'));
    return;
  }
  for (const {person, adminRoot, pendingUntil: pendingPassword} of users) {
    const owner = person === 'local-owner';
    const role = owner ? 'Installation owner' : adminRoot ? 'Administrator' : 'Member';
    const item = row('access', rowLabel(person, person === currentPerson ? `${role} · You` : role));
    if (mode !== 'managed') continue;
    const actions = element('div', '', 'row-actions');
    if (!owner) actionButton(actions, 'Change role', () => changeRole({person, adminRoot}));
    if (!owner || person === currentPerson) {
      if (pendingPassword) actionButton(actions, 'Confirm password',
        () => confirmCredential('New password', person, 'Activate new password', (password) => activatePassword(person, password)));
      else actionButton(actions, 'Reset password', () => resetPassword(person));
    }
    if (!owner) {
      const remove = actionButton(actions, 'Remove', () => confirmSetting(`Remove access for ${person}?`,
        'This person will no longer be able to sign in to this Flock.', 'Remove access',
        () => saveInstallation('access', {operation: 'remove', person})));
      remove.classList.add('quiet-danger');
    }
    if (actions.childElementCount) item.append(actions);
  }
}

const sessionReadOptions = {off: 'Off', on_demand: 'When needed', prefill: 'Before answering'};

function editSystem(key, state) {
  const isPort = key === 'port';
  const field = isPort
    ? settingField('value', 'Preferred port', {type: 'number', value: state.saved.port, placeholder: 'Use default', wide: true})
    : settingField('value', 'When Bots read T3 threads', {
      value: state.saved.sessionRead ?? '', choices: {'': 'Use default', ...sessionReadOptions}, wide: true,
    });
  const form = buildForm([field], 'Save', ({value}) =>
    saveInstallation('system', {[key]: value ? (isPort ? Number(value) : value) : null}));
  openEditor(isPort ? 'Web port' : 'Read T3 threads',
    isPort ? 'Flock will use this port after restart. Leave it blank to use the default.'
      : 'Bots can read the T3 threads of their own work when a report isn’t enough. Changes apply after restart.', form);
}

function systemValues(title, current, next, pending) {
  if (!pending) return rowLabel(title, String(current));
  const values = element('span', '', 'system-values');
  values.append(String(current), ' → ', element('b', String(next)), ' after restart');
  return rowLabel(title, values);
}

function renderSystem(state) {
  $('system').replaceChildren();
  if (!state) {
    row('system', rowLabel('Server settings unavailable', 'Sign in as an installation administrator to manage this server.'));
    return;
  }
  const {active, next, overrides, pending} = state;
  const port = row('system', systemValues('Web port', active.port || 'Automatic', next.port || 'Automatic', pending.port));
  if (overrides.port) port.append(statusBadge('Set at launch'));
  else actionButton(port, 'Change', () => editSystem('port', state)).setAttribute('aria-label', 'Change Web port');
  const sessionRead = row('system', systemValues('Read T3 threads', sessionReadOptions[active.sessionRead],
    sessionReadOptions[next.sessionRead], pending.sessionRead));
  if (overrides.sessionRead) sessionRead.append(statusBadge('Set at launch'));
  else actionButton(sessionRead, 'Change', () => editSystem('sessionRead', state))
    .setAttribute('aria-label', 'Change when Bots read T3 threads');
  if (pending.port || pending.sessionRead) restartNotice('system');
}

export function showInstallationUnavailable() {
  $('integrations-section').hidden = true;
  $('tools-section').hidden = true;
  renderAccess();
  renderSystem();
}

export async function loadInstallation() {
  const response = await fetch('/api/settings/installation', {cache: 'no-store'});
  if (response.status === 401 || response.status === 403) {
    showInstallationUnavailable();
    return;
  }
  if (!response.ok) throw new Error('Could not load installation settings.');
  installation = await response.json();
  renderIntegrations(installation.connections);
  renderAccess(installation.access);
  renderSystem(installation.system);
}
