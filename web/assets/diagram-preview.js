const systemTheme = matchMedia('(prefers-color-scheme: dark)');
const inlineMinimumHeight = 160;

class DiagramPreview extends HTMLElement {
  #path;
  #title;
  #expand;
  #close;
  #lifetime;

  constructor({ previewPath, title }, { expand, close }) {
    super();
    this.#path = previewPath;
    this.#title = title;
    this.#expand = expand;
    this.#close = close;
  }

  connectedCallback() {
    this.#lifetime = new AbortController();
    void this.#load(this.#lifetime.signal);
  }

  disconnectedCallback() {
    this.#lifetime.abort();
  }

  async #load(signal) {
    const status = document.createElement('p');
    status.className = 'diagram-status';
    status.setAttribute('role', 'status');
    status.textContent = 'Loading diagram…';
    this.classList.remove('diagram-unavailable');
    this.replaceChildren(status);
    if (this.#expand) {
      this.#expand.disabled = true;
      this.#expand.hidden = false;
    }
    const unavailable = () => {
      status.textContent = 'Diagram unavailable.';
      this.classList.add('diagram-unavailable');
      this.replaceChildren(status);
      if (this.#expand) this.#expand.hidden = true;
    };
    try {
      const response = await fetch(this.#path, { method: 'HEAD', credentials: 'same-origin', redirect: 'error', signal });
      if (signal.aborted) return;
      if (!response.ok) return unavailable();

      const frame = document.createElement('iframe');
      frame.className = 'diagram-frame';
      frame.title = this.#title;
      frame.setAttribute('sandbox', 'allow-same-origin');
      frame.referrerPolicy = 'no-referrer';
      frame.inert = Boolean(this.#expand);
      frame.addEventListener('load', () => {
        const doc = frame.contentDocument;
        if (!doc || doc.URL !== frame.src || doc.contentType !== 'text/html') return unavailable();
        if (this.#expand) {
          this.#fitInline(frame, doc, signal);
          this.#expand.disabled = false;
        } else {
          const preventNavigation = (event) => {
            if (event.target.closest?.('a, area')) event.preventDefault();
          };
          doc.addEventListener('click', preventNavigation, { capture: true, signal });
          doc.addEventListener('auxclick', preventNavigation, { capture: true, signal });
          doc.addEventListener('keydown', (event) => {
            if (event.key !== 'Escape') return;
            event.preventDefault();
            this.#close();
          }, { signal });
        }
      }, { once: true, signal });
      frame.src = this.#path;
      this.replaceChildren(frame);
    } catch {
      if (!signal.aborted) unavailable();
    }
  }

  #fitInline(frame, doc, signal) {
    let lastWidth = 0;
    let pendingFit = 0;
    const fit = () => {
      pendingFit = 0;
      const width = this.clientWidth;
      if (!width) return;
      lastWidth = width;
      // Measure at a usable minimum so viewport-relative content also fits the inline card.
      frame.style.height = inlineMinimumHeight + 'px';
      const height = Math.max(inlineMinimumHeight, doc.documentElement.scrollHeight, doc.body?.scrollHeight ?? 0);
      frame.style.height = `${height}px`;
      this.dispatchEvent(new Event('diagram-layout'));
    };
    const scheduleFit = () => { if (!pendingFit) pendingFit = requestAnimationFrame(fit); };
    const resize = new ResizeObserver(() => {
      if (lastWidth !== this.clientWidth) scheduleFit();
    });
    resize.observe(this);
    document.addEventListener('flock-theme', scheduleFit, { signal });
    systemTheme.addEventListener('change', scheduleFit, { signal });
    doc.addEventListener('load', scheduleFit, { capture: true, signal });
    signal.addEventListener('abort', () => {
      resize.disconnect();
      cancelAnimationFrame(pendingFit);
    }, { once: true });
    fit();
  }
}

customElements.define('flock-diagram-preview', DiagramPreview);

function diagramPreview(selection, actions) {
  const preview = new DiagramPreview(selection, actions);
  preview.className = 'diagram-preview';
  return preview;
}

function makeElement(tag, className) {
  const element = document.createElement(tag);
  element.className = className;
  return element;
}

function control(className, label, click) {
  const button = makeElement('button', className);
  button.type = 'button';
  button.setAttribute('aria-label', label);
  button.addEventListener('click', click);
  return button;
}

export function diagramCard(artifact, { icon, open, onLayout }) {
  const title = artifact.title || 'Diagram';
  const selection = { kind: 'diagram', previewPath: artifact.previewPath, title };
  const card = makeElement('figure', 'conversation-diagram');
  const caption = makeElement('figcaption', 'diagram-caption');
  const label = makeElement('span', 'diagram-title');
  label.textContent = title;
  const expand = control('text-button diagram-expand', `Expand ${title}`, () => open(selection));
  expand.disabled = true;
  expand.dataset.timelineAction = `expand:${artifact.id}`;
  expand.setAttribute('aria-haspopup', 'dialog');
  expand.append(icon('expand'), document.createTextNode('Expand'));
  caption.append(icon('diagram'), label, expand);
  const preview = diagramPreview(selection, { expand });
  preview.addEventListener('diagram-layout', onLayout);
  card.append(caption, preview);
  return card;
}

export function expandedDiagram(selection, icon, close) {
  const preview = diagramPreview(selection, { close });
  const actions = makeElement('div', 'diagram-controls');
  const magnification = makeElement('div', 'diagram-magnification');
  magnification.setAttribute('role', 'group');
  magnification.setAttribute('aria-label', 'Diagram zoom');
  const iconControl = (name, label, click) => {
    const button = control('icon-button', label, click);
    button.title = label;
    button.append(icon(name));
    return button;
  };
  let zoom = 100;
  const lower = 50;
  const upper = 200;
  const setZoom = (percent) => {
    zoom = percent;
    preview.style.setProperty('--diagram-zoom', zoom / 100);
    zoomOut.disabled = zoom === lower;
    zoomIn.disabled = zoom === upper;
    reset.textContent = `${zoom}%`;
    reset.setAttribute('aria-description', `Current zoom ${zoom}%`);
  };
  const zoomOut = iconControl('minus', 'Zoom out', () => setZoom(Math.max(lower, zoom - 25)));
  const zoomIn = iconControl('plus', 'Zoom in', () => setZoom(Math.min(upper, zoom + 25)));
  const reset = control('text-button diagram-zoom-reset', 'Reset zoom', () => setZoom(100));
  setZoom(100);
  const size = iconControl('expand', 'Full screen', () => {
    const fullscreen = actions.classList.toggle('diagram-viewer-fullscreen');
    const label = fullscreen ? 'Exit full screen' : 'Full screen';
    size.setAttribute('aria-label', label);
    size.title = label;
    size.replaceChildren(icon(fullscreen ? 'contract' : 'expand'));
  });
  size.classList.add('diagram-fullscreen-toggle');
  magnification.append(zoomOut, reset, zoomIn);
  actions.append(magnification, size);
  return [actions, preview];
}
