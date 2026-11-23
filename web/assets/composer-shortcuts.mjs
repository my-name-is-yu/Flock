function isSoftKeyboardEnter(event, browser) {
  const { navigator, visualViewport, innerHeight } = browser;
  if (navigator.virtualKeyboard?.boundingRect.height > 0) return true;
  if (!navigator.maxTouchPoints) return false;

  // A software keyboard may produce Enter without a physical key code.
  if (!event.code || event.code === 'Unidentified') return true;
  // Undo pinch zoom before checking for the space occupied by a docked keyboard.
  return !!visualViewport && innerHeight - visualViewport.height * visualViewport.scale > 150;
}

// Safari can report the IME's confirmation as Enter with keyCode 229.
const availableKey = (event) => !event.isComposing && event.keyCode !== 229 && !event.defaultPrevented;

export function isComposerSuggestionKey(event, browser = window) {
  if (!availableKey(event) || event.repeat || event.shiftKey || event.altKey || event.ctrlKey || event.metaKey) return false;
  return event.key === 'Tab' || (event.key === 'Enter' && !isSoftKeyboardEnter(event, browser));
}

export function handleComposerEnter(event, send, browser = window) {
  if (!availableKey(event) || event.key !== 'Enter' || event.shiftKey || event.altKey || (event.ctrlKey && event.metaKey)) return;
  if (!event.ctrlKey && !event.metaKey && isSoftKeyboardEnter(event, browser)) return;

  event.preventDefault();
  if (!event.repeat && !send.disabled) send.click();
}
