export function createBotWorkspaceCard(saveWorkspace, loadEnvironments) {
  const element = document.createElement('section');
  element.className = 'settings-card settings-workspace';
  element.setAttribute('aria-labelledby', 'settings-workspace-label');
  element.innerHTML = `
    <div class="settings-card-header">
      <h3 class="settings-card-label" id="settings-workspace-label">Where it works</h3>
      <button type="button" class="text-button settings-card-edit" aria-label="Edit where this Bot works">Edit</button>
    </div>
    <p class="settings-workspace-missing">Choose a T3 project before this Bot can start work.</p>
    <dl class="settings-workspace-facts" hidden>
      <dt>Environment</dt><dd></dd>
      <dt>Project</dt><dd></dd>
    </dl>
    <form class="settings-workspace-form" hidden>
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
  const [environmentText, projectText] = facts.querySelectorAll('dd');
  const form = element.querySelector('form');
  const fields = [...form.querySelectorAll('input')];
  const [environment, project] = fields;
  const suggestions = form.querySelector('datalist');
  const cancel = form.querySelector('button[type="button"]');
  const save = form.querySelector('button[type="submit"]');
  const feedback = element.querySelector('.settings-card-feedback');
  let workspace;
  let draft;

  function showView() {
    const configured = !!(workspace.environmentId && workspace.projectId);
    element.classList.toggle('needs-setup', !configured);
    edit.textContent = configured ? 'Edit' : 'Set up';
    edit.setAttribute('aria-label', `${edit.textContent} where this Bot works`);
    edit.hidden = !!draft;
    form.hidden = !draft;
    facts.hidden = !!draft || !configured;
    missing.hidden = !!draft || configured;
    environmentText.textContent = workspace.environmentId;
    projectText.textContent = workspace.projectId;
  }
  function update({ environmentId = '', projectId = '' }) {
    if (workspace?.environmentId === environmentId && workspace.projectId === projectId) return;
    workspace = { environmentId, projectId };
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
  edit.addEventListener('click', () => {
    draft = { initial: workspace, environmentEdited: false };
    for (const field of fields) {
      field.value = workspace[field.name];
      field.setCustomValidity('');
    }
    suggestions.replaceChildren();
    showFeedback('');
    showView();
    (environment.value ? project : environment).focus();
    void suggestEnvironments(draft);
  });
  for (const field of fields) field.addEventListener('input', () => field.setCustomValidity(''));
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
    const patch = {};
    for (const field of fields) {
      const value = field.value.trim();
      field.setCustomValidity(value ? '' : 'Enter the ID shown in T3.');
      if (value !== draft.initial[field.name]) patch[field.name] = value;
    }
    if (!form.reportValidity()) return;
    if (!Object.keys(patch).length) {
      finishEditing();
      return;
    }
    const controls = [...fields, save, cancel];
    for (const control of controls) control.disabled = true;
    showFeedback('');
    try {
      update(await saveWorkspace(patch));
      finishEditing();
      showFeedback('Work location updated.');
    } catch (error) {
      showFeedback(error.message === 'invalid_workspace'
        ? 'Check the environment and project IDs shown in T3.'
        : 'Couldn’t save where this Bot works. Try again.', true);
    } finally {
      for (const control of controls) control.disabled = false;
      if (draft && document.activeElement === document.body) project.focus();
    }
  });
  return { element, update };
}
