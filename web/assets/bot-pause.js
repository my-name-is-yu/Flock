import { accentColor } from '/avatars.js';
import { enableBackdropDismiss } from '/dialog-backdrop.mjs';

export function mountBotPause(dialog, save) {
  const title = dialog.querySelector('#bot-pause-title');
  const description = dialog.querySelector('#bot-pause-description');
  const feedback = dialog.querySelector('#bot-pause-feedback');
  const confirm = dialog.querySelector('#bot-pause-confirm');
  const cancel = dialog.querySelector('#bot-pause-cancel');
  let restoreFocus;

  function open(bot, restore) {
    if (!bot.canDelete || dialog.open) return;
    restoreFocus = restore;
    const paused = !bot.paused;
    const action = paused ? 'Pause' : 'Resume';
    dialog.style.setProperty('--bot-accent', accentColor(bot));
    title.textContent = `${action} ${bot.name}?`;
    description.textContent = paused
      ? `${bot.name} stops its open requests, won’t take new messages, and skips its recurring requests until you resume it.`
      : `${bot.name} will take new messages and run its recurring requests again.`;
    confirm.textContent = `${action} Bot`;
    feedback.textContent = '';
    confirm.onclick = async () => {
      cancel.disabled = confirm.disabled = true;
      feedback.textContent = '';
      try {
        await save(bot, paused);
        dialog.close();
      } catch {
        feedback.textContent = `Could not ${action.toLowerCase()} this Bot. Try again.`;
      } finally {
        cancel.disabled = confirm.disabled = false;
        if (dialog.open) confirm.focus();
      }
    };
    dialog.showModal();
  }

  dialog.addEventListener('cancel', (event) => {
    if (confirm.disabled) event.preventDefault();
  });
  enableBackdropDismiss(dialog, { canDismiss: () => !confirm.disabled });
  cancel.addEventListener('click', () => dialog.close());
  dialog.addEventListener('close', () => {
    if (dialog.open) return;
    restoreFocus?.();
    restoreFocus = null;
  });

  return { open, isOpen: () => dialog.open };
}
