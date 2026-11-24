export function createBotWorkspaceCard(saveSettings, loadEnvironments, loadModels) {
  const element = document.createElement('section');
  element.className = 'settings-card settings-workspace';
  element.setAttribute('aria-labelledby', 'settings-workspace-label');
  element.innerHTML = `
    <div class="settings-card-header">
      <h3 class="settings-card-label" id="settings-workspace-label">How it runs</h3>
      <button type="button" class="text-button settings-card-edit">Edit</button>
    </div>
    <p class="settings-workspace-missing">Set the model and T3 project before this Bot can start work.</p>
    <dl class="settings-workspace-facts" hidden>
      <dt>Model</dt><dd></dd>
      <dt>Environment</dt><dd></dd>
      <dt>Project</dt><dd></dd>
    </dl>
    <form class="settings-workspace-form" hidden>
      <label class="settings-workspace-field">Model
        <select name="model" aria-describedby="settings-model-feedback"></select>
      </label>
      <p class="settings-workspace-hint" id="settings-model-feedback" role="status" hidden></p>
      <label class="settings-workspace-field">Environment
        <input name="environmentId" list="settings-t3-environments" maxlength="256" required autocomplete="off" spellcheck="false">
      </label>
      <datalist id="settings-t3-environments"></datalist>
      <label class="settings-workspace-field">Project
        <input name="projectId" maxlength="256" required autocomplete="off" spellcheck="false">
      </label>
      <p class="settings-workspace-hint">Use the environment and project IDs shown in T3.</p>
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
  const [modelText, environmentText, projectText] = facts.querySelectorAll('dd');
  const form = element.querySelector('form');
  const [environment, project] = form.querySelectorAll('input');
  const model = form.elements.model;
  const modelFeedback = form.querySelector('#settings-model-feedback');
  const fields = [model, environment, project];
  const suggestions = form.querySelector('datalist');
  const cancel = form.querySelector('button[type="button"]');
  const save = form.querySelector('button[type="submit"]');
  const feedback = element.querySelector('.settings-card-feedback');
  let workspace;
  let draft;
  let catalog = [];

  function showView() {
    const configured = !!(workspace.model && workspace.environmentId && workspace.projectId);
    element.classList.toggle('needs-setup', !configured);
    edit.textContent = configured ? 'Edit' : 'Set up';
    edit.setAttribute('aria-label', `${edit.textContent} how this Bot runs`);
    edit.hidden = !!draft;
    form.hidden = !draft;
    facts.hidden = !!draft || !configured;
    missing.hidden = !!draft || configured;
    modelText.textContent = catalog.find(({ slug }) => slug === workspace.model)?.displayName ?? workspace.model;
    environmentText.textContent = workspace.environmentId;
    projectText.textContent = workspace.projectId;
  }
  function update({ model = '', environmentId = '', projectId = '' }) {
    if (workspace?.model === model && workspace.environmentId === environmentId && workspace.projectId === projectId) return;
    workspace = { model, environmentId, projectId };
    showView();
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
    suggestions.replaceChildren(...environments.map((id) => new Option(id)));
    if (!opened.environmentEdited && !environment.value && environments.length === 1) {
      environment.value = environments[0];
      environment.setCustomValidity('');
    }
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
  async function refreshModels(opened) {
    const choices = await loadModels().catch(() => null);
    if (draft !== opened) return;
    if (choices?.length) {
      catalog = choices;
      showModelOptions(choices, model.value, 'Choose a model');
      showView();
    } else {
      showModelOptions([], model.value, 'No models available');
      modelFeedback.textContent = choices ? 'No models are available for this connection.'
        : 'Couldn’t load models. Check the ChatGPT connection in Flock settings, then reopen this editor.';
      modelFeedback.hidden = false;
    }
  }
  edit.addEventListener('click', () => {
    draft = { initial: workspace, environmentEdited: false };
    showModelOptions([], workspace.model, 'Loading models…');
    modelFeedback.hidden = true;
    for (const field of [environment, project]) {
      field.value = workspace[field.name];
      field.setCustomValidity('');
    }
    suggestions.replaceChildren();
    showFeedback('');
    showView();
    (fields.find((field) => !field.value) ?? model).focus();
    void suggestEnvironments(draft);
    void refreshModels(draft);
  });
  for (const field of [environment, project]) field.addEventListener('input', () => field.setCustomValidity(''));
  environment.addEventListener('input', () => { if (draft) draft.environmentEdited = true; });
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
    for (const field of [environment, project])
      field.setCustomValidity(field.value.trim() ? '' : 'Enter the ID shown in T3.');
    if (!form.reportValidity()) return;
    const patch = {};
    for (const field of fields) {
      const value = field.value.trim();
      if (value !== draft.initial[field.name]) patch[field.name] = value;
    }
    if (!Object.keys(patch).length) {
      finishEditing();
      return;
    }
    const controls = [...fields, save, cancel];
    for (const control of controls) control.disabled = true;
    showFeedback('');
    try {
      update(await saveSettings(patch));
      finishEditing();
      showFeedback('Settings saved.');
    } catch (error) {
      showFeedback(error.message === 'invalid_workspace'
        ? 'Check the environment and project IDs shown in T3.'
        : 'Couldn’t save these settings. Try again.', true);
    } finally {
      for (const control of controls) control.disabled = false;
      if (draft && document.activeElement === document.body) project.focus();
    }
  });
  return { element, update };
}
