export function enableBackdropDismiss(dialog, { canDismiss = () => true, close = () => dialog.close() } = {}) {
  let pointerDownInside = false;

  const isOutside = (event) => {
    if (event.target !== dialog) return false;
    const rect = dialog.getBoundingClientRect();
    return rect.width > 0 && rect.height > 0 &&
      (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom);
  };

  dialog.addEventListener('pointerdown', (event) => {
    pointerDownInside = !isOutside(event);
  });
  dialog.addEventListener('click', (event) => {
    const startedInside = pointerDownInside;
    pointerDownInside = false;
    if (!startedInside && isOutside(event) && canDismiss()) close();
  });
  dialog.addEventListener('close', () => {
    pointerDownInside = false;
  });
}
