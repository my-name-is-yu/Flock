import assert from 'node:assert/strict';
import { assetUrl, dumpFixture } from './dom-harness.mjs';

const output = await dumpFixture(process.argv[2], `<!doctype html><html><body><script type="module">
  import { createBotRoleCard } from ${JSON.stringify(assetUrl('bot-role.js'))};
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
assert.match(output, /<html[^>]*data-role-description="Guides this Bot and the Sessions it starts\."/);
