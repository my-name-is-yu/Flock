import { accentColor } from '/avatars.js';

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
      ? 'Pausing stops open requests, refuses messages, and skips scheduled work until you resume this Bot.'
      : 'This Bot will accept new messages and run future scheduled work.';
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
  cancel.addEventListener('click', () => dialog.close());
  dialog.addEventListener('close', () => {
    if (dialog.open) return;
    restoreFocus?.();
    restoreFocus = null;
  });

  return { open, isOpen: () => dialog.open };
}
