function isSoftKeyboardEnter(event, browser) {
  const { navigator, visualViewport, innerHeight } = browser;
  if (navigator.virtualKeyboard?.boundingRect.height > 0) return true;
  if (!navigator.maxTouchPoints) return false;

  // A software keyboard may produce Enter without a physical key code.
  if (!event.code || event.code === 'Unidentified') return true;
  // Undo pinch zoom before checking for the space occupied by a docked keyboard.
  return !!visualViewport && innerHeight - visualViewport.height * visualViewport.scale > 150;
}

export function handleComposerEnter(event, send, browser = window) {
  // Safari can report the IME's confirmation as Enter with keyCode 229.
  if (event.isComposing || event.keyCode === 229 || event.defaultPrevented) return;
  if (event.key !== 'Enter' || event.shiftKey || event.altKey || (event.ctrlKey && event.metaKey)) return;
  if (!event.ctrlKey && !event.metaKey && isSoftKeyboardEnter(event, browser)) return;

  event.preventDefault();
  if (!event.repeat && !send.disabled) send.click();
}
