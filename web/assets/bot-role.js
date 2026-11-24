export function createBotRoleCard(saveRole) {
  const element = document.createElement('section');
  element.className = 'settings-card settings-role';
  element.setAttribute('aria-labelledby', 'settings-role-label');
  element.innerHTML = `
    <div class="settings-card-header">
      <h3 class="settings-card-label" id="settings-role-label">Role</h3>
      <button type="button" class="text-button settings-card-edit" aria-label="Edit role">Edit</button>
    </div>
    <p class="settings-soul" id="settings-role-text" data-folded="true"></p>
    <button type="button" class="text-button settings-soul-toggle" aria-expanded="false"
      aria-controls="settings-role-text" hidden>Show all</button>
    <form class="settings-role-form" hidden>
      <textarea class="settings-role-input" rows="8" aria-labelledby="settings-role-label"
        placeholder="For example: Reviews release branches, runs the checks, and reports what blocks shipping."></textarea>
      <div class="settings-card-actions">
        <button type="button" class="dialog-button quiet">Cancel</button>
        <button type="submit" class="dialog-button primary">Save</button>
      </div>
    </form>
    <p class="settings-card-feedback" role="status"></p>
  `;
  const edit = element.querySelector('.settings-card-edit');
  const preview = element.querySelector('.settings-soul');
  const toggle = element.querySelector('.settings-soul-toggle');
  const form = element.querySelector('form');
  const input = form.querySelector('textarea');
  const cancel = form.querySelector('button[type="button"]');
  const save = form.querySelector('button[type="submit"]');
  const feedback = element.querySelector('.settings-card-feedback');
  let text;
  let originalSoul;
  let initialText;

  function measurePreview() {
    const lineHeight = parseFloat(getComputedStyle(preview).lineHeight);
    toggle.hidden = !text || !form.hidden || preview.scrollHeight <= Math.ceil(8 * lineHeight);
  }
  function setExpanded(expanded) {
    preview.dataset.folded = String(!expanded);
    toggle.textContent = expanded ? 'Show less' : 'Show all';
    toggle.setAttribute('aria-expanded', String(expanded));
  }
  function update(next) {
    if (text === next) return;
    text = next;
    preview.textContent = text || 'Describe what this Bot does and how it decides.';
    preview.classList.toggle('settings-soul-empty', !text);
    setExpanded(false);
    measurePreview();
  }
  const size = new ResizeObserver(measurePreview);
  size.observe(preview);
  toggle.addEventListener('click', () => setExpanded(preview.dataset.folded === 'true'));

  function showFeedback(message, error = false) {
    feedback.classList.toggle('error', error);
    feedback.textContent = message;
  }
  function finishEditing() {
    const active = document.activeElement;
    const restoreFocus = active === document.body || element.contains(active);
    form.hidden = true;
    edit.hidden = preview.hidden = false;
    measurePreview();
    if (restoreFocus) edit.focus();
  }
  edit.addEventListener('click', () => {
    showFeedback('');
    edit.hidden = preview.hidden = toggle.hidden = true;
    form.hidden = false;
    originalSoul = text;
    input.value = originalSoul;
    initialText = input.value;
    input.focus();
  });
  cancel.addEventListener('click', () => {
    showFeedback('');
    finishEditing();
  });
  input.addEventListener('keydown', (event) => {
    if (event.isComposing) return;
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      cancel.click();
    } else if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      save.click();
    }
  });
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (save.disabled) return;
    if (input.value === initialText) {
      finishEditing();
      return;
    }
    input.disabled = save.disabled = cancel.disabled = true;
    showFeedback('');
    try {
      update(await saveRole(input.value, originalSoul));
      finishEditing();
      showFeedback('Role updated');
    } catch (error) {
      const message = {
        invalid_soul: 'The role is too long. Shorten it and try again.',
        role_changed: 'The role changed or contains hidden text. Your edits weren’t saved.',
        role_contains_secrets: 'Remove passwords or keys from the role before saving.',
      }[error.message] ?? 'Couldn’t save the role.';
      showFeedback(message, true);
    } finally {
      input.disabled = save.disabled = cancel.disabled = false;
      if (!form.hidden && document.activeElement === document.body) input.focus();
    }
  });
  return { element, update, dispose: () => size.disconnect() };
}
