export function settingsNavigation(conversations = '/') {
  const nav = document.querySelector('.settings-nav');
  nav.innerHTML = `
    <a class="settings-back" href="/"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m15 18-6-6 6-6"/></svg><span>Conversations</span></a>
    <a href="/settings#appearance" data-section="appearance"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.4 1.4m11.2 11.2L19 19M5 19l1.4-1.4m11.2-11.2L19 5"/></svg>Appearance</a>
    <a href="/settings#permissions" data-section="permissions"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3.5 5 6.5v5c0 4.2 2.9 7.6 7 9 4.1-1.4 7-4.8 7-9v-5z"/><path d="m9 12 2 2 4-4"/></svg>Permissions</a>
    <a href="/settings#bots" data-section="bots"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="4.5" y="6" width="15" height="12.5" rx="4"/><path d="M12 6V3.5M9.5 12.2v.6M14.5 12.2v.6"/></svg>Bots</a>
    <a href="/settings#connections" data-section="connections"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 15 15 9"/><path d="M10.5 6.5 12 5a4.2 4.2 0 0 1 6 6l-1.5 1.5M13.5 17.5 12 19a4.2 4.2 0 0 1-6-6l1.5-1.5"/></svg>Connections</a>
    <a href="/settings#access" data-section="access"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="10" cy="7.5" r="3"/><path d="M4.5 19v-2a5.5 5.5 0 0 1 11 0v2M17 5a3 3 0 0 1 0 5.7M18 13a4.2 4.2 0 0 1 2.5 4v2"/></svg>Access</a>
    <a href="/settings#system" data-section="system"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3.5" y="4.5" width="17" height="12" rx="2"/><path d="M8.5 20h7M12 16.5V20M7.5 10.5h2.5"/></svg>System</a>
  `;
  nav.firstElementChild.href = conversations;
  return new Map([...nav.querySelectorAll('[data-section]')].map(link => [link.dataset.section, link]));
}

// A plain "Saved." only confirms what the control already shows, so screen readers hear it
// but it doesn't push the page down. Errors and messages with more to say stay visible.
export function quietSavedStatus(status) {
  if (!status) return;
  const sync = () => status.classList.toggle('sr-only', status.textContent === 'Saved.');
  new MutationObserver(sync).observe(status, { childList: true, characterData: true, subtree: true });
  sync();
}

export function setSettingsStatus(statusEl, text, isError = false) {
  if (!statusEl) return;
  statusEl.textContent = text;
  statusEl.classList.toggle('error', Boolean(isError && text));
}

export function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text) node.textContent = text;
  if (className) node.className = className;
  return node;
}

export function rowLabel(title, ...detail) {
  const label = element('div', '', 'row-label');
  label.append(element('strong', title));
  if (detail.length) {
    const description = element('small');
    description.append(...detail);
    label.append(description);
  }
  return label;
}

export function row(list, content) {
  const item = element('div', '', 'settings-row');
  item.append(content);
  (typeof list === 'string' ? document.getElementById(list) : list).append(item);
  return item;
}

export function actionButton(item, label, onclick, primary = false) {
  const control = element('button', label, primary ? 'button primary' : 'button');
  control.type = 'button';
  control.onclick = onclick;
  item.append(control);
  return control;
}
