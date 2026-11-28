export function node(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text != null) element.textContent = text;
  return element;
}

export function setMessageBody(element, html, plain) {
  if (typeof html !== 'string') {
    element.textContent = plain ?? '';
    return;
  }
  element.classList.add('markdown-body');
  element.innerHTML = html;
  for (const link of element.querySelectorAll('a[href]')) {
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
  }
}
