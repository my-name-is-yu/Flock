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
  const card = createBotWorkspaceCard(async (patch) => {
    patches.push(patch);
    if (patches.length === 1) throw new Error('invalid_workspace');
    return { environmentId: 'new-environment', ...patch };
  }, async () => [{ environmentId: 'suggested-environment', authorization: null }]);
  const edit = card.element.querySelector('.settings-card-edit');
  const form = card.element.querySelector('form');
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
  check(environment.value === 'suggested-environment', 'Single connected environment did not prefill');
  cancel.click();

  card.update({ environmentId: 'original-environment', projectId: 'original-project' });
  edit.click();
  project.value = 'draft-project';
  project.focus();
  placeRows(body, [document.createElement('header'), card.element]);
  card.update({ environmentId: 'new-environment', projectId: 'original-project' });
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
  check(form.hidden && facts[0].textContent === 'new-environment' && facts[1].textContent === 'draft-project',
    'Save did not show the current workspace');
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
