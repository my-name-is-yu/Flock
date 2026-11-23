const systemTheme = matchMedia('(prefers-color-scheme: dark)');
const maximumDiagramHeight = 20000;
const maximumDiagramWidth = 4096;

class DiagramPreview extends HTMLElement {
  #path;
  #title;
  #expand;
  #lifetime;

  constructor(path, title, expand) {
    super();
    this.#path = path;
    this.#title = title;
    this.#expand = expand;
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
      // The host measures the document. The frame cannot run scripts or receive input.
      frame.setAttribute('sandbox', 'allow-same-origin');
      frame.referrerPolicy = 'no-referrer';
      frame.inert = true;
      frame.addEventListener('load', () => {
        const doc = frame.contentDocument;
        if (!doc || doc.URL !== frame.src || doc.contentType !== 'text/html') return unavailable();
        this.#fitFrame(frame, doc, signal);
        if (this.#expand) this.#expand.disabled = false;
      }, { once: true, signal });
      frame.src = this.#path;
      this.replaceChildren(frame);
    } catch {
      if (!signal.aborted) unavailable();
    }
  }

  #fitFrame(frame, doc, signal) {
    let lastWidth = 0;
    let pendingFit = 0;
    const fit = () => {
      pendingFit = 0;
      const width = this.clientWidth;
      if (!width) return;
      lastWidth = width;
      frame.style.width = `${width}px`;
      // Measuring from a fixed viewport lets pages shrink on reflow and avoids a feedback loop with vh-based styles.
      frame.style.height = '1px';
      const root = doc.documentElement;
      const body = doc.body;
      if (!this.#expand) {
        const contentWidth = Math.max(root.scrollWidth, body?.scrollWidth ?? 0);
        frame.style.width = `${Math.max(width, Math.min(maximumDiagramWidth, contentWidth))}px`;
      }
      const height = Math.max(1, root.scrollHeight, body?.scrollHeight ?? 0);
      frame.style.height = `${Math.min(maximumDiagramHeight, height)}px`;
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

export function diagramPreview({ replyId, artifactId, title }, expand = null) {
  const path = `/api/messages/${replyId}/artifacts/${encodeURIComponent(artifactId)}`;
  const preview = new DiagramPreview(path, title, expand);
  preview.className = 'diagram-preview';
  if (!expand) {
    preview.tabIndex = 0;
    preview.setAttribute('role', 'region');
    preview.setAttribute('aria-label', title);
  }
  return preview;
}
