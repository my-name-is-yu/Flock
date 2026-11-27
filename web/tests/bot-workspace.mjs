import { assetUrl, dumpFixture, requirePass } from './dom-harness.mjs';

async function exercise(createBotWorkspaceCard, placeRows) {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const settle = () => new Promise((resolve) => setTimeout(resolve, 0));
  const body = document.querySelector('main');
  const patches = [];
  const catalog = {
    models: [
      { slug: 'next-model', displayName: 'Next model', reasoningEfforts: ['low', 'ultra'] },
      { slug: 'no-reasoning', displayName: 'No reasoning', reasoningEfforts: [] },
    ],
    fallbackReasoningEfforts: ['low', 'medium', 'high'],
  };
  let catalogCalls = 0;
  let saved = { environmentId: 'new-environment', model: 'new-saved-model' };
  const card = createBotWorkspaceCard(async (patch) => {
    patches.push(patch);
    if (patches.length === 1) throw new Error('invalid_workspace');
    saved = { ...saved, ...patch };
    return saved;
  }, async (environmentId) => environmentId
    ? { projects: [{ id: 'initial-project' }, { id: 'draft-project', name: 'Draft project' }] }
    : { environments: [{ id: 'suggested-environment', name: 'Suggested' }] }, async () => {
    if (++catalogCalls === 1) throw new Error('models_unavailable');
    return catalog;
  });
  const edit = card.element.querySelector('.settings-card-edit');
  const form = card.element.querySelector('form');
  const model = form.elements.model;
  const effort = form.elements.reasoningEffort;
  const effortOptions = () => Array.from(effort.options, (option) => option.value).join(',');
  const chooseModel = (value) => {
    model.value = value;
    model.dispatchEvent(new Event('change'));
  };
  const choose = async (select, value) => {
    select.value = value;
    select.dispatchEvent(new Event('change'));
    await settle();
  };
  const environment = form.elements.environmentId;
  const project = form.elements.projectId;
  const save = form.querySelector('button[type="submit"]');
  const cancel = form.querySelector('button[type="button"]');

  card.update({});
  placeRows(body, [document.createElement('header'), card.element]);
  edit.click();
  check(edit.hidden && form.hidden, 'A viewer could open the workspace editor');
  card.update({}, true);
  edit.click();
  await settle();
  check(!form.elements.modelCallLimit && !card.element.textContent.includes('Steps per request'),
    'The workspace still displays a step limit');
  check(!environment.value && Array.from(environment.options).some((option) => option.value === 'suggested-environment') &&
    project.disabled, 'The connected environment should be offered without pinning it');
  check(!form.querySelector('#settings-model-feedback').hidden, 'A catalog failure did not show feedback');
  cancel.click();

  edit.click();
  await settle();
  check(catalogCalls === 2 && Array.from(model.options).some((option) => option.value === 'next-model'),
    'Reopening did not load the catalog after a connection failure');
  await choose(environment, 'suggested-environment');
  await choose(project, 'initial-project');
  check(!project.disabled && project.value === 'initial-project', 'Choosing an environment did not load its projects');
  save.click();
  await settle();
  check(patches.length === 0 && !form.hidden && model.validity.valueMissing,
    'Bot setup submitted without a model');
  cancel.click();

  card.update({ environmentId: 'original-environment', projectId: 'original-project', model: 'unlisted-model' });
  edit.click();
  await settle();
  check(catalogCalls === 3 && model.value === 'unlisted-model' &&
    Array.from(model.options).some((option) => option.value === 'next-model'),
    'Reopening did not retry the catalog or preserve the saved model outside the catalog');
  check(effortOptions() === ',low,medium,high', 'A model without catalog metadata did not offer fallback efforts');
  check(environment.value === 'original-environment' && project.value === 'original-project',
    'Saved locations outside the T3 list were not kept');
  await choose(project, 'draft-project');
  project.focus();
  placeRows(body, [document.createElement('header'), card.element]);
  card.update({ ...saved, projectId: 'original-project' });
  check(!form.hidden && project.value === 'draft-project' && document.activeElement === project,
    'A settings refresh replaced the draft or moved focus');

  save.click();
  await settle();
  check(!form.hidden && project.value === 'draft-project' && !save.disabled, 'A failed save discarded the draft');
  save.click();
  await settle();
  check(patches.length === 2 && patches.every((patch) => JSON.stringify(patch) === '{"projectId":"draft-project"}'),
    'Save sent an unchanged field');
  const facts = card.element.querySelectorAll('dd');
  check(form.hidden && facts[0].textContent === 'new-saved-model' && facts[1].textContent === 'Model default' &&
    facts.length === 4 && facts[2].textContent === 'new-environment' && facts[3].textContent === 'draft-project',
    'Save did not show the current settings');

  edit.click();
  await settle();
  chooseModel('next-model');
  model.focus();
  placeRows(body, [document.createElement('header'), card.element]);
  card.update({ ...saved, model: 'external-model' });
  check(model.value === 'next-model' && document.activeElement === model,
    'A settings refresh replaced the model draft or moved focus');
  save.click();
  await settle();
  check(JSON.stringify(patches[2]) === '{"model":"next-model"}' && facts[0].textContent === 'Next model',
    'Model-only save sent location fields or did not show the model name');

  edit.click();
  await settle();
  project.value = '';
  save.click();
  await settle();
  check(!form.hidden && project.validity.valueMissing && patches.length === 3,
    'Clearing an existing project could disable configured schedules');
  cancel.click();

  saved = { model: 'personal-model' };
  card.update(saved);
  edit.click();
  await settle();
  chooseModel('next-model');
  await choose(environment, 'suggested-environment');
  save.click();
  await settle();
  check(!form.hidden && !project.validity.valid && patches.length === 3,
    'An environment without a project was submitted');
  await choose(environment, '');
  check(project.disabled && !project.value, 'Clearing the environment kept a project');
  save.click();
  await settle();
  check(JSON.stringify(patches[3]) === '{"model":"next-model"}' && form.hidden && !facts[0].parentElement.hidden &&
    facts[0].textContent === 'Next model', 'An unpinned Bot could not save and display a model by itself');

  edit.click();
  await settle();
  check(effortOptions() === ',low,ultra', 'Advertised efforts were replaced by the fallback list');
  effort.value = 'ultra';
  save.click();
  await settle();
  check(JSON.stringify(patches[4]) === '{"reasoningEffort":"ultra"}' && facts[1].textContent === 'Ultra',
    'Saving a reasoning effort changed another field or failed to show it');

  edit.click();
  await settle();
  check(effort.value === 'ultra', 'Reopening did not show the saved effort');
  chooseModel('no-reasoning');
  check(effortOptions() === '' && effort.value === '', 'A model with no advertised levels offered an override');
  save.click();
  await settle();
  check(JSON.stringify(patches[5]) === '{"model":"no-reasoning","reasoningEffort":""}' &&
    facts[1].textContent === 'Model default', 'Changing the model left an unsupported effort saved');

  let viewModelLoads = 0;
  const viewer = createBotWorkspaceCard(async () => ({}), async () => ({ environments: [] }), async () => {
    viewModelLoads++;
    return catalog;
  });
  const current = { model: 'next-model' };
  const modelLabel = viewer.element.querySelector('.settings-workspace-facts dd');
  viewer.update(current);
  await settle();
  check(viewModelLoads === 0 && modelLabel.textContent === 'next-model',
    'Read-only settings requested the editor-only model catalog');
  viewer.update(current, true);
  await settle();
  check(viewModelLoads === 1 && modelLabel.textContent === 'Next model',
    'Editable settings did not show the model name before opening the editor');
}

const html = `<!doctype html><html><body><main></main>
  <script type="module">
    import { createBotWorkspaceCard } from ${JSON.stringify(assetUrl('bot-workspace.js'))};
    import { placeRows } from ${JSON.stringify(assetUrl('timeline-rows.js'))};
    try {
      await (${exercise})(createBotWorkspaceCard, placeRows);
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script></body></html>`;
requirePass(await dumpFixture(process.argv[2], html), 'The browser did not finish the workspace checks');
