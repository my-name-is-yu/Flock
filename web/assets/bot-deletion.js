import { enableBackdropDismiss } from './dialog-backdrop.mjs';

const errors = new Map([
  ['bot_busy', 'Finish or stop this Bot’s active work, then try again.'],
  ['access_denied', 'You don’t have permission to delete this Bot.'],
]);

export function mountBotDeletion(dialog, { remove, onDeleted, focusFallback }) {
  const title = dialog.querySelector('#bot-delete-title');
  const feedback = dialog.querySelector('#bot-delete-feedback');
  const cancel = dialog.querySelector('#bot-delete-cancel');
  const confirm = dialog.querySelector('#bot-delete-confirm');
  let target;

  function showFeedback(text, isError = false) {
    feedback.classList.toggle('error', isError);
    feedback.textContent = text;
  }

  function open(bot, origin) {
    if (!bot?.canDelete || dialog.open) return;
    target = { id: bot.id, origin };
    title.textContent = `Delete ${bot.name}?`;
    showFeedback('');
    origin?.focus({ preventScroll: true });
    dialog.showModal();
  }

  cancel.addEventListener('click', () => dialog.close());
  dialog.addEventListener('cancel', (event) => {
    if (confirm.disabled) event.preventDefault();
  });
  enableBackdropDismiss(dialog, { canDismiss: () => !confirm.disabled });
  dialog.addEventListener('close', () => {
    if (dialog.open || !target) return;
    const { origin } = target;
    target = null;
    if (origin?.isConnected) {
      origin.focus();
      if (dialog.ownerDocument.activeElement === origin) return;
    }
    focusFallback();
  });
  confirm.addEventListener('click', async () => {
    if (!target || confirm.disabled) return;
    const { id } = target;
    cancel.disabled = confirm.disabled = true;
    showFeedback('Deleting Bot…');
    let errorCode;
    try { await remove(id); }
    catch (error) { errorCode = error?.message || 'unknown'; }
    cancel.disabled = confirm.disabled = false;
    if (errorCode && errorCode !== 'bot_not_found') {
      showFeedback(errors.get(errorCode) ?? 'Couldn’t delete this Bot. Try again.', true);
      confirm.focus();
      return;
    }
    onDeleted(id);
    dialog.close();
  });

  return { open, isOpen: () => dialog.open };
}
