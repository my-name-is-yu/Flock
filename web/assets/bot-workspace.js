function effortLabel(effort) {
  if (!effort) return 'Model default';
  if (effort === 'xhigh') return 'Extra high';
  if (effort === 'max') return 'Maximum';
  return effort[0].toUpperCase() + effort.slice(1);
}

export function createBotWorkspaceCard(saveSettings, loadEnvironments, loadModels) {
  const element = document.createElement('section');
  element.className = 'settings-card settings-workspace';
  element.setAttribute('aria-labelledby', 'settings-workspace-label');
  element.innerHTML = `
    <div class="settings-card-header">
      <h3 class="settings-card-label" id="settings-workspace-label">How it runs</h3>
      <button type="button" class="text-button settings-card-edit">Edit</button>
    </div>
    <p class="settings-workspace-missing">Choose a model before this Bot can start work.</p>
    <dl class="settings-workspace-facts" hidden>
      <dt>Model</dt><dd></dd>
      <dt>Reasoning effort</dt><dd></dd>
      <dt>Steps per request</dt><dd></dd>
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
      <label class="settings-workspace-field">Steps per request
        <input name="modelCallLimit" type="number" min="1" max="2147483647" required aria-describedby="settings-steps-hint">
      </label>
      <p class="settings-workspace-hint" id="settings-steps-hint">Each request starts with this many steps. You can approve more as needed.</p>
      <label class="settings-workspace-field">T3 environment
        <input name="environmentId" list="settings-t3-environments" maxlength="256" autocomplete="off" spellcheck="false">
      </label>
      <datalist id="settings-t3-environments"></datalist>
      <label class="settings-workspace-field">T3 project
        <input name="projectId" maxlength="256" autocomplete="off" spellcheck="false">
      </label>
      <p class="settings-workspace-hint">Where this Bot’s work runs. Copy both IDs from T3, or leave them empty and the Bot picks or asks.</p>
      <div class="settings-card-actions">
        <button type="button" class="dialog-button quiet">Cancel</button>
        <button type="submit" class="dialog-button primary">Save</button>
      </div>
    </form>
    <p class="settings-card-feedback" role="status"></p>
  `;
  const edit = element.querySelector('.settings-card-edit');
  const missing = element.querySelector('.settings-workspace-missing');
  const facts = element.querySelector('.settings-workspace-facts');
  const [modelText, effortText, allowanceText, environmentText, projectText] = facts.querySelectorAll('dd');
  const form = element.querySelector('form');
  const { model, reasoningEffort, modelCallLimit: allowance, environmentId: environment, projectId: project } = form.elements;
  const modelFeedback = form.querySelector('#settings-model-feedback');
  const suggestions = form.querySelector('datalist');
  const cancel = form.querySelector('button[type="button"]');
  const save = form.querySelector('button[type="submit"]');
  const feedback = element.querySelector('.settings-card-feedback');
  let workspace;
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
    allowanceText.textContent = workspace.modelCallLimit || 'Not set';
    for (const [text, value] of [[environmentText, workspace.environmentId], [projectText, workspace.projectId]]) {
      text.textContent = value || 'Chosen per request';
      text.classList.toggle('unset', !value);
    }
  }
  function update({ model = '', reasoningEffort = '', modelCallLimit = '', environmentId = '', projectId = '' }, canEdit = editable) {
    const current = { model, reasoningEffort, modelCallLimit, environmentId, projectId };
    if (editable === canEdit && workspace && Object.keys(current).every((key) => workspace[key] === current[key])) return;
    editable = canEdit;
    workspace = current;
    if (!editable) draft = null;
    showView();
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
  async function suggestEnvironments(opened) {
    const environments = await loadEnvironments().catch(() => []);
    if (draft !== opened) return;
    suggestions.replaceChildren(...environments.map(({ environmentId }) => new Option(environmentId)));
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
    allowance.value = workspace.modelCallLimit;
    showEffortOptions(workspace.reasoningEffort, true);
    modelFeedback.hidden = true;
    for (const field of [environment, project]) {
      field.value = workspace[field.name];
      field.required = !!field.value;
      field.setCustomValidity('');
    }
    suggestions.replaceChildren();
    showFeedback('');
    showView();
    model.focus();
    void suggestEnvironments(draft);
    void refreshModels(draft);
  });
  model.addEventListener('change', () => showEffortOptions(reasoningEffort.value));
  for (const field of [environment, project]) field.addEventListener('input', () => {
    environment.setCustomValidity('');
    project.setCustomValidity('');
  });
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
    const environmentId = environment.value.trim();
    const projectId = project.value.trim();
    environment.setCustomValidity(!environmentId && (projectId || environment.value)
      ? 'Enter the environment ID shown in T3.' : '');
    project.setCustomValidity(!projectId && project.value ? 'Enter the project ID shown in T3.' : '');
    if (!form.reportValidity()) return;
    const patch = {};
    const edits = { model: model.value, reasoningEffort: reasoningEffort.value, modelCallLimit: allowance.valueAsNumber,
      environmentId, projectId };
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
        invalid_workspace: 'Check the environment and project IDs shown in T3.',
        invalid_reasoning_effort: 'Choose an effort this model supports or use Model default.',
        invalid_model_call_limit: 'Enter a whole number between 1 and 2,147,483,647.',
      };
      showFeedback(messages[error.message] ?? 'Couldn’t save these settings. Try again.', true);
    } finally {
      for (const control of form.elements) control.disabled = false;
      if (draft && document.activeElement === document.body) project.focus();
    }
  });
  return { element, update };
}
