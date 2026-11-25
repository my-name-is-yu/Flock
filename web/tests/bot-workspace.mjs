import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

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
  let saved = { environmentId: 'new-environment', model: 'new-saved-model', modelCallLimit: 20 };
  const card = createBotWorkspaceCard(async (patch) => {
    patches.push(patch);
    if (patches.length === 1) throw new Error('invalid_workspace');
    saved = { ...saved, ...patch };
    return saved;
  }, async () => [{ environmentId: 'suggested-environment', authorization: null }], async () => {
    if (++catalogCalls === 1) throw new Error('models_unavailable');
    return catalog;
  });
  const edit = card.element.querySelector('.settings-card-edit');
  const form = card.element.querySelector('form');
  const model = form.elements.model;
  const allowance = form.elements.modelCallLimit;
  const effort = form.elements.reasoningEffort;
  const effortOptions = () => Array.from(effort.options, (option) => option.value).join(',');
  const chooseModel = (value) => {
    model.value = value;
    model.dispatchEvent(new Event('change'));
  };
  const environment = form.elements.environmentId;
  const project = form.elements.projectId;
  const save = form.querySelector('button[type="submit"]');
  const cancel = form.querySelector('button[type="button"]');

  card.update({});
  placeRows(body, [document.createElement('header'), card.element]);
  edit.click();
  check(edit.hidden && form.hidden, 'A viewer could open the workspace editor');
  card.update({ modelCallLimit: 20 }, true);
  edit.click();
  await settle();
  check(!environment.value && form.querySelector('datalist option').value === 'suggested-environment',
    'The connected environment should be suggested without pinning it');
  check(!form.querySelector('#settings-model-feedback').hidden, 'A catalog failure did not show feedback');
  check(allowance.valueAsNumber === 20, 'The default allowance was missing from the editor');
  cancel.click();

  edit.click();
  await settle();
  check(catalogCalls === 2 && Array.from(model.options).some((option) => option.value === 'next-model'),
    'Reopening did not load the catalog after a connection failure');
  environment.value = 'suggested-environment';
  project.value = 'initial-project';
  save.click();
  await settle();
  check(patches.length === 0 && !form.hidden && model.validity.valueMissing,
    'Bot setup submitted without a model');
  cancel.click();

  card.update({ environmentId: 'original-environment', projectId: 'original-project', model: 'unlisted-model', modelCallLimit: 20 });
  edit.click();
  await settle();
  check(catalogCalls === 3 && model.value === 'unlisted-model' &&
    Array.from(model.options).some((option) => option.value === 'next-model'),
    'Reopening did not retry the catalog or preserve the saved model outside the catalog');
  check(effortOptions() === ',low,medium,high', 'A model without catalog metadata did not offer fallback efforts');
  project.value = 'draft-project';
  project.focus();
  placeRows(body, [document.createElement('header'), card.element]);
  card.update({ ...saved, projectId: 'original-project' });
  check(!form.hidden && project.value === 'draft-project' && document.activeElement === project,
    'A settings refresh replaced the draft or moved focus');

  project.value = '  ';
  save.click();
  await settle();
  check(patches.length === 0 && !project.validity.valid, 'A whitespace-only project was submitted');
  project.value = ' draft-project ';
  project.dispatchEvent(new Event('input'));
  save.click();
  await settle();
  check(!form.hidden && project.value === ' draft-project ' && !save.disabled, 'A failed save discarded the draft');
  save.click();
  await settle();
  check(patches.length === 2 && patches.every((patch) => JSON.stringify(patch) === '{"projectId":"draft-project"}'),
    'Save sent an unchanged field or did not trim the draft');
  const facts = card.element.querySelectorAll('dd');
  check(form.hidden && facts[0].textContent === 'new-saved-model' && facts[1].textContent === 'Model default' &&
    facts[2].textContent === '20' && facts[3].textContent === 'new-environment' && facts[4].textContent === 'draft-project',
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

  saved = { model: 'personal-model', modelCallLimit: 20 };
  card.update(saved);
  edit.click();
  await settle();
  chooseModel('next-model');
  project.value = 'project-without-environment';
  save.click();
  await settle();
  check(!form.hidden && !environment.validity.valid && patches.length === 3,
    'A project without an environment was submitted');
  project.value = '';
  project.dispatchEvent(new Event('input'));
  save.click();
  await settle();
  check(JSON.stringify(patches[3]) === '{"model":"next-model"}' && form.hidden && !facts[0].parentElement.hidden &&
    facts[0].textContent === 'Next model', 'An unpinned Bot could not save and display a model by itself');

  edit.click();
  await settle();
  allowance.value = '0';
  save.click();
  await settle();
  check(!form.hidden && allowance.validity.rangeUnderflow && patches.length === 4,
    'A zero allowance was submitted');
  allowance.value = '12';
  save.click();
  await settle();
  check(JSON.stringify(patches[4]) === '{"modelCallLimit":12}' && form.hidden && facts[2].textContent === '12',
    'An allowance-only save changed other fields or did not show the saved value');
  edit.click();
  await settle();
  check(effortOptions() === ',low,ultra', 'Advertised efforts were replaced by the fallback list');
  effort.value = 'ultra';
  save.click();
  await settle();
  check(JSON.stringify(patches[5]) === '{"reasoningEffort":"ultra"}' && facts[1].textContent === 'Ultra',
    'Saving a reasoning effort changed another field or failed to show it');

  edit.click();
  await settle();
  check(effort.value === 'ultra', 'Reopening did not show the saved effort');
  chooseModel('no-reasoning');
  check(effortOptions() === '' && effort.value === '', 'A model with no advertised levels offered an override');
  save.click();
  await settle();
  check(JSON.stringify(patches[6]) === '{"model":"no-reasoning","reasoningEffort":""}' &&
    facts[1].textContent === 'Model default', 'Changing the model left an unsupported effort saved');
}

const directory = mkdtempSync(join(tmpdir(), 'flock-bot-workspace-'));
try {
  const workspace = new URL('../assets/bot-workspace.js', import.meta.url);
  const rows = new URL('../assets/timeline-rows.js', import.meta.url);
  const html = `<!doctype html><html><body><main></main>
    <script type="module">
      import { createBotWorkspaceCard } from ${JSON.stringify(workspace.href)};
      import { placeRows } from ${JSON.stringify(rows.href)};
      try {
        await (${exercise})(createBotWorkspaceCard, placeRows);
        document.documentElement.dataset.flockResult = 'pass';
      } catch (error) {
        document.documentElement.dataset.flockResult = error.message;
      }
    </script></body></html>`;
  const fixture = join(directory, 'index.html');
  writeFileSync(fixture, html);
  const output = execFileSync(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--allow-file-access-from-files', `--user-data-dir=${directory}`,
    '--virtual-time-budget=3000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
  const result = output.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the workspace checks');
} finally {
  rmSync(directory, { recursive: true, force: true });
}
