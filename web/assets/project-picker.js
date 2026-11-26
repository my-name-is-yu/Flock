export function createProjectPicker(fieldset, loadWorkspaces, onLoadingChange) {
  const [environment, project] = fieldset.querySelectorAll('select');
  const feedback = fieldset.querySelector('[role="status"]');
  let loadId = 0;

  function fill(select, prompt, items = []) {
    select.replaceChildren(new Option(prompt, ''), ...items.map(({ id, name }) => new Option(name || id, id)));
    select.disabled = !items.length;
  }

  function status(message, loading = false) {
    feedback.textContent = message;
    feedback.hidden = !message;
    onLoadingChange(loading);
  }

  const unavailable = 'Couldn’t load T3 workspaces. You can choose a project in Bot settings later.';

  async function loadProjects(preferred = '') {
    const id = ++loadId;
    const environmentId = environment.value;
    fill(project, 'No project');
    if (!environmentId) {
      status('');
      return;
    }
    status('Loading T3 workspaces…', true);
    try {
      const { projects } = await loadWorkspaces(environmentId);
      if (id !== loadId) return;
      fill(project, 'No project', projects);
      if (projects.some(({ id }) => id === preferred)) project.value = preferred;
      status('');
    } catch {
      if (id === loadId) status(unavailable);
    }
  }

  async function open() {
    fieldset.hidden = false;
    const id = ++loadId;
    fill(environment, 'No environment');
    fill(project, 'No project');
    status('Loading T3 workspaces…', true);
    try {
      const { environments, recent } = await loadWorkspaces();
      if (id !== loadId) return;
      fill(environment, 'No environment', environments);
      if (!environments.length) {
        status('Connect T3 in Flock settings to choose a project.');
        return;
      }
      const hasRecent = environments.some(({ id }) => id === recent?.environmentId);
      environment.value = hasRecent ? recent.environmentId : environments.length === 1 ? environments[0].id : '';
      void loadProjects(hasRecent ? recent.projectId : '');
    } catch {
      if (id === loadId) status(unavailable);
    }
  }

  environment.addEventListener('change', () => void loadProjects());

  return {
    open,
    close() {
      ++loadId;
      fieldset.hidden = true;
      status('');
    },
    destination() {
      return !fieldset.hidden && project.value ? { environmentId: environment.value, projectId: project.value } : {};
    },
  };
}
