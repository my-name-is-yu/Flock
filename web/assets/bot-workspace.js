function effortLabel(effort) {
  if (!effort) return 'Model default';
  if (effort === 'xhigh') return 'Extra high';
  if (effort === 'max') return 'Maximum';
  return effort[0].toUpperCase() + effort.slice(1);
}

export function createBotWorkspaceCard(saveSettings, loadWorkspaces, loadModels) {
  const element = document.createElement('section');
  element.className = 'settings-card settings-workspace';
  element.setAttribute('aria-labelledby', 'settings-workspace-label');
  element.innerHTML = `
    <div class="settings-card-header">
      <h3 class="settings-card-label" id="settings-workspace-label">How it runs</h3>
      <button type="button" class="button settings-card-edit">Edit</button>
    </div>
    <p class="settings-workspace-missing">Choose a model so this Bot can reply.</p>
    <dl class="settings-workspace-facts" hidden>
      <dt>Model</dt><dd></dd>
      <dt>Reasoning effort</dt><dd></dd>
      <dt>T3 environment</dt><dd></dd>
      <dt>T3 project</dt><dd></dd>
    </dl>
    <form class="settings-workspace-form" hidden>
      <div class="settings-workspace-model">
        <label class="settings-workspace-field">Model
          <select name="model" required aria-describedby="settings-model-feedback"></select>
        </label>
        <label class="settings-workspace-field">Reasoning effort
          <select name="reasoningEffort"></select>
        </label>
      </div>
      <p class="settings-workspace-hint" id="settings-model-feedback" role="status" hidden></p>
      <label class="settings-workspace-field">T3 environment
        <select name="environmentId"></select>
      </label>
      <label class="settings-workspace-field">T3 project
        <select name="projectId"></select>
      </label>
      <p class="settings-workspace-hint" id="settings-workspace-feedback" role="status">Where this Bot’s work runs. Keep “Chosen per request” and the Bot picks or asks.</p>
      <div class="settings-card-actions">
        <button type="button" class="button quiet">Cancel</button>
        <button type="submit" class="button primary">Save</button>
      </div>
    </form>
    <p class="settings-card-feedback" role="status"></p>
  `;
  const edit = element.querySelector('.settings-card-edit');
  const missing = element.querySelector('.settings-workspace-missing');
  const facts = element.querySelector('.settings-workspace-facts');
  const [modelText, effortText, environmentText, projectText] = facts.querySelectorAll('dd');
  const form = element.querySelector('form');
  const { model, reasoningEffort, environmentId: environment, projectId: project } = form.elements;
  const modelFeedback = form.querySelector('#settings-model-feedback');
  const workspaceFeedback = form.querySelector('#settings-workspace-feedback');
  const workspaceHint = workspaceFeedback.textContent;
  const cancel = form.querySelector('button[type="button"]');
  const save = form.querySelector('button[type="submit"]');
  const feedback = element.querySelector('.settings-card-feedback');
  let workspace;
  const placeNames = new Map();
  let draft;
  let catalog = { models: [], fallbackReasoningEfforts: [] };
  let editable = false;
  let catalogRequested = false;

  function catalogModel(slug) {
    return catalog.models.find((model) => model.slug === slug);
  }
  function showView() {
    const configured = !!workspace.model;
    element.classList.toggle('needs-setup', !configured);
    edit.textContent = configured ? 'Edit' : 'Set up';
    edit.setAttribute('aria-label', `${edit.textContent} how this Bot runs`);
    edit.hidden = !editable || !!draft;
    form.hidden = !draft;
    facts.hidden = !!draft || !configured;
    missing.hidden = !!draft || configured;
    modelText.textContent = catalogModel(workspace.model)?.displayName ?? workspace.model;
    effortText.textContent = effortLabel(workspace.reasoningEffort);
    const places = [
      [environmentText, workspace.environmentId, placeNames.get(`environment:${workspace.environmentId}`)],
      [projectText, workspace.projectId, placeNames.get(`project:${workspace.environmentId}/${workspace.projectId}`)],
    ];
    for (const [text, value, name] of places) {
      text.textContent = name || value || 'Chosen per request';
      text.classList.toggle('unset', !value);
      text.classList.toggle('place-name', !!name);
    }
  }
  function update({ model = '', reasoningEffort = '', environmentId = '', projectId = '' }, canEdit = editable) {
    const current = { model, reasoningEffort, environmentId, projectId };
    if (editable === canEdit && workspace && Object.keys(current).every((key) => workspace[key] === current[key])) return;
    editable = canEdit;
    workspace = current;
    if (!editable) draft = null;
    showView();
    if (editable && workspace.environmentId && !placeNames.has(`environment:${workspace.environmentId}`)) void loadNames(workspace);
    if (editable && workspace.model && !catalogRequested && !catalog.models.length) {
      catalogRequested = true;
      loadModels().then((data) => {
        if (catalog.models.length || !data?.models?.length) return;
        catalog = { models: data.models, fallbackReasoningEfforts: data.fallbackReasoningEfforts ?? [] };
        if (!draft) showView();
      }).catch(() => {});
    }
  }
  function showFeedback(message, error = false) {
    feedback.classList.toggle('error', error);
    feedback.textContent = message;
  }
  function finishEditing() {
    const active = document.activeElement;
    draft = null;
    showView();
    if (active === document.body || element.contains(active)) edit.focus();
  }
  function rememberNames(kind, items, environmentId = '') {
    for (const { id, name } of items) {
      if (name) placeNames.set(kind === 'project' ? `project:${environmentId}/${id}` : `environment:${id}`, name);
    }
  }
  async function loadNames(expected) {
    const environments = await loadWorkspaces().catch(() => null);
    if (environments) rememberNames('environment', environments.environments ?? []);
    if (expected.environmentId) {
      const projects = await loadWorkspaces(expected.environmentId).catch(() => null);
      if (projects) rememberNames('project', projects.projects ?? [], expected.environmentId);
    }
    if (workspace === expected && !draft) showView();
  }
  function fillPlaces(select, items, selected) {
    const options = [new Option('Chosen per request', ''), ...items.map(({ id, name }) => new Option(name || id, id))];
    if (selected && !items.some(({ id }) => id === selected)) options.push(new Option(selected, selected));
    select.replaceChildren(...options);
    select.value = selected;
  }
  async function loadProjects(opened, selected) {
    const environmentId = environment.value;
    fillPlaces(project, [], environmentId ? selected : '');
    project.disabled = !environmentId;
    if (!environmentId) return;
    const data = await loadWorkspaces(environmentId).catch(() => null);
    if (draft !== opened || environment.value !== environmentId) return;
    rememberNames('project', data?.projects ?? [], environmentId);
    fillPlaces(project, data?.projects ?? [], project.value);
    if (!data) workspaceFeedback.textContent = 'Couldn’t load T3 projects. Check the T3 connection in Flock settings.';
  }
  async function loadPlaces(opened) {
    const data = await loadWorkspaces().catch(() => null);
    if (draft !== opened) return;
    rememberNames('environment', data?.environments ?? []);
    fillPlaces(environment, data?.environments ?? [], environment.value);
    if (!data) workspaceFeedback.textContent = 'Couldn’t load T3 environments. Check the T3 connection in Flock settings.';
    else await loadProjects(opened, opened.initial.projectId);
  }
  function showModelOptions(choices, selected, prompt) {
    const placeholder = new Option(prompt, '');
    placeholder.disabled = true;
    const options = choices.map(({ slug, displayName }) => new Option(displayName, slug));
    if (selected && !choices.some(({ slug }) => slug === selected))
      options.unshift(new Option(selected, selected));
    model.replaceChildren(placeholder, ...options);
    model.value = selected;
  }
  function showEffortOptions(selected, preserveSaved = false) {
    const efforts = model.value ? catalogModel(model.value)?.reasoningEfforts ?? catalog.fallbackReasoningEfforts : [];
    const options = [new Option(effortLabel(''), ''), ...efforts.map((effort) => new Option(effortLabel(effort), effort))];
    if (selected && preserveSaved && !efforts.includes(selected)) {
      const previous = new Option(effortLabel(selected), selected);
      previous.disabled = true;
      options.push(previous);
    }
    reasoningEffort.replaceChildren(...options);
    reasoningEffort.value = options.some((option) => option.value === selected) ? selected : '';
  }
  async function refreshModels(opened) {
    const data = await loadModels().catch((error) => error.data);
    if (draft !== opened) return;
    catalog = { models: data?.models ?? [], fallbackReasoningEfforts: data?.fallbackReasoningEfforts ?? [] };
    if (catalog.models.length) {
      showModelOptions(catalog.models, model.value, 'Choose a model');
      showView();
    } else {
      showModelOptions([], model.value, 'No models available');
      modelFeedback.textContent = data?.models ? 'No models are available for this connection.'
        : 'Couldn’t load models. Check the ChatGPT connection in Flock settings, then reopen this editor.';
      modelFeedback.hidden = false;
    }
    showEffortOptions(reasoningEffort.value, model.value === opened.initial.model);
  }
  edit.addEventListener('click', () => {
    if (!editable) return;
    draft = { initial: workspace };
    showModelOptions([], workspace.model, 'Loading models…');
    showEffortOptions(workspace.reasoningEffort, true);
    modelFeedback.hidden = true;
    fillPlaces(environment, [], workspace.environmentId);
    fillPlaces(project, [], workspace.projectId);
    project.disabled = !workspace.environmentId;
    for (const field of [environment, project]) {
      field.required = !!workspace[field.name];
      field.setCustomValidity('');
    }
    workspaceFeedback.textContent = workspaceHint;
    showFeedback('');
    showView();
    model.focus();
    void loadPlaces(draft);
    void refreshModels(draft);
  });
  model.addEventListener('change', () => showEffortOptions(reasoningEffort.value));
  environment.addEventListener('change', () => {
    project.setCustomValidity('');
    void loadProjects(draft, '');
  });
  project.addEventListener('change', () => project.setCustomValidity(''));
  cancel.addEventListener('click', () => {
    showFeedback('');
    finishEditing();
  });
  form.addEventListener('keydown', (event) => {
    if (event.isComposing || event.key !== 'Escape') return;
    event.preventDefault();
    event.stopPropagation();
    cancel.click();
  });
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (!draft || save.disabled) return;
    const environmentId = environment.value;
    const projectId = environmentId ? project.value : '';
    project.setCustomValidity(environmentId && !projectId ? 'Choose a project in this environment.' : '');
    if (!form.reportValidity()) return;
    const patch = {};
    const edits = { model: model.value, reasoningEffort: reasoningEffort.value, environmentId, projectId };
    for (const [key, value] of Object.entries(edits)) {
      if (value !== draft.initial[key]) patch[key] = value;
    }
    if (patch.model && reasoningEffort.value) patch.reasoningEffort = reasoningEffort.value;
    if (!Object.keys(patch).length) {
      finishEditing();
      return;
    }
    for (const control of form.elements) control.disabled = true;
    showFeedback('');
    try {
      update(await saveSettings(patch));
      finishEditing();
      showFeedback('Settings saved.');
    } catch (error) {
      const messages = {
        invalid_workspace: 'Choose an available T3 environment and project.',
        invalid_reasoning_effort: 'Choose an effort this model supports or use Model default.',
      };
      showFeedback(messages[error.message] ?? 'Couldn’t save these settings. Try again.', true);
    } finally {
      for (const control of form.elements) control.disabled = false;
      project.disabled = !environment.value;
      if (draft && document.activeElement === document.body) project.focus();
    }
  });
  return { element, update, edit: () => { if (editable && !draft) edit.click(); } };
}
