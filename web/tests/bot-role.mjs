import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

const directory = mkdtempSync(join(tmpdir(), 'flock-bot-role-'));
try {
  const roleModule = new URL('../assets/bot-role.js', import.meta.url);
  const fixture = join(directory, 'index.html');
  writeFileSync(fixture, `<!doctype html><html><body><script type="module">
    import { createBotRoleCard } from ${JSON.stringify(roleModule.href)};
    const card = createBotRoleCard(() => {});
    document.body.append(card.element);
    card.update('Review releases.', true);
    card.element.querySelector('[aria-label="Edit role"]').click();
    const input = card.element.querySelector('textarea');
    const description = document.getElementById(input.getAttribute('aria-describedby'));
    if (!input.form.hidden && description?.checkVisibility()) {
      document.documentElement.dataset.roleDescription = description.textContent;
    }
  </script></body></html>`);
  const output = execFileSync(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--allow-file-access-from-files', `--user-data-dir=${directory}`,
    '--virtual-time-budget=3000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
  assert.match(output, /<html[^>]*data-role-description="Guides this Bot and the Sessions it starts\."/);
} finally {
  rmSync(directory, { recursive: true, force: true });
}
