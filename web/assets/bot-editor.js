import {
  AVATAR_COLORS,
  AVATAR_ICONS,
  AVATAR_SHAPES,
  resolveAvatar,
  showAvatar,
} from '/avatars.js';
import { createProjectPicker } from '/project-picker.js';
import { enableBackdropDismiss } from '/dialog-backdrop.mjs';

// A preset only seeds the new Bot's SOUL.md; what the Bot does is up to the user afterwards.
const PRESETS = [
  { id: 'reviewer', title: 'Reviewer', detail: 'Reviews changes and verifies results with evidence', icon: 'shield-check' },
  { id: 'builder', title: 'Builder', detail: 'Implements changes and runs the relevant tests', icon: 'wrench' },
  { id: 'researcher', title: 'Researcher', detail: 'Investigates questions and summarizes findings', icon: 'telescope' },
];

const RANDOM_NAMES = [
  'Moss', 'Wren', 'Juniper', 'Pip', 'Kite', 'Clover', 'Orbit', 'Fern',
  'Nimbus', 'Sol', 'Pebble', 'Scout', 'Fig', 'Ollie', 'Pogo', 'Nova',
];

const MAX_UPLOAD_BYTES = 5 * 1024 * 1024;
const ALLOWED_MIME = /^image\/(png|jpeg|webp|gif)$/;

const UI_ICONS = {
  'chevron-right': '<path d="m9.5 6.5 5.5 5.5-5.5 5.5"/>',
  plus: '<path d="M5 12h14"/><path d="M12 5v14"/>',
  'shield-check': '<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"/><path d="m9 12 2 2 4-4"/>',
  telescope: '<path d="m10.065 12.493-6.18 1.318a.934.934 0 0 1-1.108-.702l-.537-2.15a1.07 1.07 0 0 1 .691-1.265l13.504-4.44"/><path d="m13.56 11.747 4.332-.924"/><path d="m16 21-3.105-6.21"/><path d="M16.485 5.94a2 2 0 0 1 1.455-2.425l1.09-.272a1 1 0 0 1 1.212.727l1.515 6.06a1 1 0 0 1-.727 1.213l-1.09.272a2 2 0 0 1-2.425-1.455z"/><path d="m6.158 8.633 1.114 4.456"/><path d="m8 21 3.105-6.21"/><circle cx="12" cy="13" r="2"/>',
  wrench: '<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.106-3.105c.32-.322.863-.22.983.218a6 6 0 0 1-8.259 7.057l-7.91 7.91a1 1 0 0 1-2.999-3l7.91-7.91a6 6 0 0 1 7.057-8.259c.438.12.54.662.219.984z"/>',
  close: '<path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/>',
  dice: '<rect width="16" height="16" x="4" y="4" rx="3"/><path d="M9 9h.01"/><path d="M15 9h.01"/><path d="M12 12h.01"/><path d="M9 15h.01"/><path d="M15 15h.01"/>',
  shuffle: '<path d="m18 14 4 4-4 4"/><path d="m18 2 4 4-4 4"/><path d="M2 18h1.973a4 4 0 0 0 3.3-1.7l5.454-8.6a4 4 0 0 1 3.3-1.7H22"/><path d="M2 6h1.972a4 4 0 0 1 3.6 2.2"/><path d="M22 18h-6.041a4 4 0 0 1-3.3-1.8l-.359-.45"/>',
  upload: '<path d="M12 3v12"/><path d="m17 8-5-5-5 5"/><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>',
};


function uiIcon(name) {
  return `<svg aria-hidden="true" focusable="false" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${UI_ICONS[name] ?? ''}</svg>`;
}

function pickDifferent(items, exclude) {
  const pool = items.filter((item) => item !== exclude);
  const source = pool.length ? pool : items;
  return source[Math.floor(Math.random() * source.length)];
}

function pickRandomName(currentName, existingNames = []) {
  const taken = new Set(existingNames.map((n) => n.trim().toLowerCase()));
  const current = currentName.trim().toLowerCase();
  const unused = RANDOM_NAMES.filter((name) => name.toLowerCase() !== current && !taken.has(name.toLowerCase()));
  if (unused.length) return unused[Math.floor(Math.random() * unused.length)];
  const different = RANDOM_NAMES.filter((name) => name.toLowerCase() !== current);
  return different[Math.floor(Math.random() * different.length)];
}

function readFileAsBase64(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error('Could not read the selected image.'));
    reader.onload = () => {
      const result = typeof reader.result === 'string' ? reader.result : '';
      const comma = result.indexOf(',');
      if (comma < 0) reject(new Error('Could not encode the selected image.'));
      else resolve(result.slice(comma + 1));
    };
    reader.readAsDataURL(file);
  });
}

const EASE_OUT_EXPO = 'cubic-bezier(0.23, 1, 0.32, 1)';
const EASE_SPRING = 'cubic-bezier(0.22, 1, 0.36, 1)';
const reducedMotionQuery = typeof window !== 'undefined' && typeof window.matchMedia === 'function'
  ? window.matchMedia('(prefers-reduced-motion: reduce)')
  : null;
const activeAnimations = new WeakMap();

function prefersReducedMotion() {
  return Boolean(reducedMotionQuery?.matches);
}

function runAnimation(element, keyframes, options) {
  if (!element || prefersReducedMotion() || typeof element.animate !== 'function') return null;
  const previous = activeAnimations.get(element);
  if (previous) {
    try { previous.cancel(); } catch {}
  }
  const anim = element.animate(keyframes, options);
  activeAnimations.set(element, anim);
  const cleanup = () => {
    if (activeAnimations.get(element) === anim) activeAnimations.delete(element);
  };
  anim.onfinish = cleanup;
  anim.oncancel = cleanup;
  return anim;
}

export function createBotEditor(dialog, { api, onSaved, onCancel, onDraft }) {
  dialog.classList.add('accent-scope');
  dialog.innerHTML = `
    <section class="studio-start" id="studio-start" hidden>
      <header class="studio-head">
        <h2 id="studio-start-title">New Bot</h2>
        <button type="button" class="icon-button studio-close" data-close aria-label="Close" data-tooltip="Close" data-shortcut="Esc">${uiIcon('close')}</button>
      </header>
      <div class="studio-start-list">
        <button type="button" class="studio-start-option" data-preset="custom">
          <span class="studio-start-icon">${uiIcon('plus')}</span>
          <span class="studio-start-copy"><strong>Start from scratch</strong><small>Pick a name and look now, describe its role later</small></span>
          ${uiIcon('chevron-right')}
        </button>
      </div>
      <h3 class="studio-start-heading" id="studio-presets-heading">Presets</h3>
      <div class="studio-start-list" id="studio-presets" role="group" aria-labelledby="studio-presets-heading"></div>
    </section>

    <div class="studio" id="studio-editor" hidden>
      <div class="studio-stage">
        <div class="studio-hero-avatar" id="studio-hero-avatar"></div>
        <div class="studio-name">
          <input class="studio-name-input" id="studio-bot-name" type="text" maxlength="32" autocomplete="off" spellcheck="false" placeholder="Bot name" aria-label="Name">
          <button type="button" id="studio-random-name" class="icon-button studio-name-random" aria-label="Random name" data-tooltip="Random name">${uiIcon('dice')}</button>
        </div>
      </div>

      <div class="studio-panel">
        <header class="studio-head">
          <h2 id="studio-title">New Bot</h2>
          <button type="button" class="icon-button studio-close" data-close aria-label="Close" data-tooltip="Close" data-shortcut="Esc">${uiIcon('close')}</button>
        </header>

        <div class="studio-bar">
          <div class="studio-tabs" id="studio-tabs" role="group" aria-label="Avatar source">
            <button type="button" data-kind="shape" data-label="Shapes" aria-pressed="true">Shapes</button>
            <button type="button" data-kind="icon" data-label="Icons" aria-pressed="false">Icons</button>
            <button type="button" data-kind="upload" data-label="Upload" aria-pressed="false">Upload</button>
          </div>
          <button type="button" id="studio-shuffle" class="icon-button studio-shuffle" aria-label="Shuffle appearance" data-tooltip="Shuffle appearance">${uiIcon('shuffle')}</button>
        </div>

        <div class="studio-gallery" id="studio-gallery" role="group" aria-label="Avatar choices"></div>

        <div class="studio-upload" id="studio-upload-panel" hidden>
          <label class="studio-drop" id="studio-drop" for="studio-photo">
            ${uiIcon('upload')}
            <strong id="studio-upload-label">Choose an image</strong>
            <span>PNG, JPEG, WebP or GIF, up to 5 MB</span>
            <input id="studio-photo" type="file" accept="image/png,image/jpeg,image/webp,image/gif">
          </label>
          <button class="button quiet-danger studio-remove-upload" id="studio-remove-upload" type="button" hidden>Remove image</button>
          <p class="studio-error" id="studio-upload-error" role="alert" hidden></p>
        </div>

        <div class="studio-colors-field" id="studio-color-field">
          <div class="studio-label"><span id="studio-color-label">Color</span></div>
          <div id="studio-colors" class="studio-palette" role="group" aria-labelledby="studio-color-label"></div>
        </div>

        <fieldset class="studio-workspace" hidden>
          <legend>Where does it work? <span>Optional</span></legend>
          <label class="settings-workspace-field">Environment
            <select name="environmentId"></select>
          </label>
          <label class="settings-workspace-field">Project
            <select name="projectId"></select>
          </label>
          <p class="settings-workspace-hint" role="status" hidden></p>
        </fieldset>

        <p class="studio-error" id="studio-submit-error" role="alert" hidden></p>
        <footer class="studio-footer">
          <button type="button" class="button quiet" id="studio-cancel">Cancel</button>
          <button type="button" class="button primary" id="studio-save">Create</button>
        </footer>
      </div>
    </div>
  `;

  const el = (id) => dialog.querySelector(`#${id}`);
  const els = {
    start: el('studio-start'),
    startTitle: el('studio-start-title'),
    presets: el('studio-presets'),
    studio: el('studio-editor'),
    title: el('studio-title'),
    heroAvatar: el('studio-hero-avatar'),
    nameInput: el('studio-bot-name'),
    randomName: el('studio-random-name'),
    shuffle: el('studio-shuffle'),
    tabs: el('studio-tabs'),
    gallery: el('studio-gallery'),
    uploadPanel: el('studio-upload-panel'),
    drop: el('studio-drop'),
    photo: el('studio-photo'),
    uploadLabel: el('studio-upload-label'),
    removeUpload: el('studio-remove-upload'),
    uploadError: el('studio-upload-error'),
    colorField: el('studio-color-field'),
    colors: el('studio-colors'),
    submitError: el('studio-submit-error'),
    cancel: el('studio-cancel'),
    save: el('studio-save'),
  };
  let loadingWorkspace = false;
  const workspace = createProjectPicker(dialog.querySelector('.studio-workspace'), environmentId => {
    const path = environmentId ? `settings/projects?${new URLSearchParams({ environmentId })}` : 'settings/projects';
    return api(path, undefined, { signal: AbortSignal.timeout(6500) });
  }, loading => {
    loadingWorkspace = loading;
    renderSaveButton();
  });

  for (const preset of PRESETS) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'studio-start-option';
    btn.dataset.preset = preset.id;
    btn.innerHTML = `<span class="studio-start-icon">${uiIcon(preset.icon)}</span><span class="studio-start-copy"><strong>${preset.title}</strong><small>${preset.detail}</small></span>${uiIcon('chevron-right')}`;
    els.presets.append(btn);
  }

  for (const [label, hex] of AVATAR_COLORS) {
    const swatch = document.createElement('button');
    swatch.type = 'button';
    swatch.className = 'studio-swatch';
    swatch.dataset.color = hex;
    swatch.dataset.tooltip = label;
    swatch.setAttribute('aria-label', label);
    swatch.style.setProperty('--swatch', hex);
    els.colors.append(swatch);
  }

  let mode = 'create';
  let step = 'start';
  let celebrateFrame = 0;
  let celebratedLook = '';
  let saving = false;
  let existingNames = [];
  let state = null;
  let heroHandle = null;
  let heroRenderedKind = '';
  let renderedGalleryKind = '';
  let renderedGalleryColor = '';
  let uploadGeneration = 0;
  let objectUrl = '';
  let returnFocus = null;
  let closeReason = 'cancel';

  function revokeObjectUrl() {
    if (objectUrl) {
      URL.revokeObjectURL(objectUrl);
      objectUrl = '';
    }
  }

  function destroyHero() {
    cancelAnimationFrame(celebrateFrame);
    heroHandle?.destroy();
    heroHandle = null;
    heroRenderedKind = '';
    celebratedLook = '';
  }

  function setUploadError(message) {
    els.uploadError.hidden = !message;
    els.uploadError.textContent = message;
  }

  function setSubmitError(message) {
    els.submitError.hidden = !message;
    els.submitError.textContent = message;
  }

  function currentPreviewImageUrl() {
    return state.uploadPreviewUrl || state.existingImageUrl || '';
  }

  function currentDraftAvatar() {
    // Until the user touches the look, show (and keep) the stored avatar exactly, custom shapes included.
    if (state.originalAvatar && !state.lookChanged) return state.originalAvatar;
    if (state.kind === 'upload') {
      const imageUrl = currentPreviewImageUrl();
      if (imageUrl) {
        return { kind: 'upload', imageUrl, filename: state.filename, color: state.color };
      }
      return { kind: 'shape', example: state.shape, color: state.color };
    }
    if (state.kind === 'icon') {
      return { kind: 'icon', icon: state.icon, color: state.color };
    }
    return { kind: 'shape', example: state.shape, color: state.color };
  }

  function previewBot(avatarOverride) {
    return {
      id: state.botId || 'preview-bot',
      name: state.name.trim() || 'Untitled bot',
      avatar: avatarOverride ?? currentDraftAvatar(),
    };
  }

  // Picking a look plays Open Bot Avatar's "done" celebration. A mount in "done" starts at rest,
  // so step through idle for a frame to make the animator see a fresh state.
  function celebrate() {
    if (!heroHandle) return;
    cancelAnimationFrame(celebrateFrame);
    heroHandle.update({ state: 'idle' });
    celebrateFrame = requestAnimationFrame(() => {
      celebrateFrame = requestAnimationFrame(() => heroHandle?.update({ state: 'done' }));
    });
  }

  function popHeroAvatar() {
    runAnimation(
      els.heroAvatar,
      [
        { transform: 'scale(0.92)' },
        { transform: 'scale(1.04)', offset: 0.58 },
        { transform: 'scale(1)' },
      ],
      { duration: 280, easing: EASE_SPRING },
    );
  }

  function animateShuffleIcon() {
    runAnimation(
      els.shuffle.querySelector('svg'),
      [
        { transform: 'rotate(0deg) scale(1)' },
        { transform: 'rotate(180deg) scale(1.16)', offset: 0.52 },
        { transform: 'rotate(372deg) scale(0.96)', offset: 0.82 },
        { transform: 'rotate(360deg) scale(1)' },
      ],
      { duration: 380, easing: EASE_SPRING },
    );
  }

  function animateDiceIcon() {
    runAnimation(
      els.randomName.querySelector('svg'),
      [
        { transform: 'rotate(0deg) scale(1)' },
        { transform: 'rotate(-20deg) scale(1.14) translateY(-1px)', offset: 0.22 },
        { transform: 'rotate(24deg) scale(1.16) translateY(0px)', offset: 0.5 },
        { transform: 'rotate(-10deg) scale(1.04)', offset: 0.76 },
        { transform: 'rotate(0deg) scale(1)' },
      ],
      { duration: 360, easing: EASE_SPRING },
    );
    runAnimation(
      els.nameInput,
      [
        { opacity: 0.55, transform: 'translateY(2px) scale(0.985)' },
        { opacity: 1, transform: 'none' },
      ],
      { duration: 180, easing: EASE_OUT_EXPO },
    );
  }

  function animatePanelEnter(panel, duration = 210) {
    runAnimation(
      panel,
      [
        { opacity: 0, transform: 'translateY(6px)' },
        { opacity: 1, transform: 'none' },
      ],
      { duration, easing: EASE_OUT_EXPO },
    );
  }

  function animateGalleryChoices() {
    if (prefersReducedMotion()) return;
    const choices = els.gallery.querySelectorAll('.studio-choice');
    choices.forEach((choice, index) => {
      const selected = choice.getAttribute('aria-pressed') === 'true';
      runAnimation(
        choice,
        [
          { opacity: 0, transform: 'scale(0.9) translateY(4px)' },
          { opacity: 1, transform: selected ? 'scale(1.08) translateY(-1px)' : 'scale(1.03) translateY(-1px)', offset: 0.72 },
          { opacity: 1, transform: selected ? 'scale(1.05)' : 'none' },
        ],
        {
          duration: 240,
          delay: index * 22,
          easing: EASE_OUT_EXPO,
          fill: 'backwards',
        },
      );
    });
  }

  function saveLabel() {
    if (saving) return mode === 'edit' ? 'Saving…' : 'Creating…';
    return mode === 'edit' ? 'Save' : 'Create';
  }

  function emitDraft() {
    onDraft?.({ mode, botId: state.botId, ...previewBot() });
  }

  function renderSaveButton() {
    const hasValidImage = state.kind !== 'upload' || !!currentPreviewImageUrl();
    els.save.disabled = saving || loadingWorkspace || !state.name.trim() || !hasValidImage;
    els.save.textContent = saveLabel();
  }

  function renderTextPreview() {
    renderSaveButton();
    emitDraft();
  }

  function renderPreviews() {
    const avatar = currentDraftAvatar();
    const isShapePreview = avatar.kind === 'shape';
    if (isShapePreview) {
      const resolved = resolveAvatar(previewBot(avatar));
      if (heroHandle && heroRenderedKind === 'shape') {
        heroHandle.update({
          definition: resolved.shape.definition,
          color: resolved.shape.color,
          eyeColor: resolved.shape.eyeColor,
        });
        els.heroAvatar.classList.toggle('mug-outline', resolved.shape.color.toLowerCase() === '#e1dfdf' || resolved.shape.color.toLowerCase() === '#f4f4f4');
      } else {
        destroyHero();
        heroHandle = showAvatar(els.heroAvatar, previewBot(avatar), {
          still: false,
          state: 'idle',
          followPointer: true,
        });
        heroRenderedKind = 'shape';
      }
      const look = `${avatar.example}|${avatar.color}`;
      if (look !== celebratedLook) {
        celebratedLook = look;
        if (state.lookChanged) celebrate();
      }
    } else {
      destroyHero();
      showAvatar(els.heroAvatar, previewBot(avatar), { still: true });
      heroRenderedKind = avatar.kind;
    }
  }

  function renderGallery({ animateChoices = false } = {}) {
    const kind = state.kind;
    const kindChanged = renderedGalleryKind !== kind;
    if (kindChanged) {
      els.gallery.replaceChildren();
      const entries = kind === 'shape' ? AVATAR_SHAPES : AVATAR_ICONS;
      if (kind !== 'upload') {
        for (const [value, label] of entries) {
          const btn = document.createElement('button');
          btn.type = 'button';
          btn.className = 'studio-choice';
          btn.dataset.value = value;
          btn.dataset.tooltip = label;
          btn.setAttribute('aria-label', label);
          const mini = document.createElement('span');
          mini.className = 'studio-mini';
          btn.append(mini);
          els.gallery.append(btn);
        }
        els.gallery.style.setProperty('--choices', String(entries.length));
      }
    }
    for (const btn of els.gallery.querySelectorAll('button')) {
      const value = btn.dataset.value;
      const selected = (kind === 'shape' ? state.shape : state.icon) === value;
      btn.setAttribute('aria-pressed', String(selected));
      if (kindChanged || renderedGalleryColor !== state.color) {
        const optionAvatar = kind === 'shape'
          ? { kind: 'shape', example: value, color: state.color }
          : { kind: 'icon', icon: value, color: state.color };
        showAvatar(btn.firstElementChild, previewBot(optionAvatar), { still: true });
      }
    }
    renderedGalleryKind = kind;
    renderedGalleryColor = state.color;
    els.gallery.hidden = kind === 'upload';
    if ((kindChanged || animateChoices) && kind !== 'upload') {
      animateGalleryChoices();
    }
  }

  function syncTabsIndicator(animate = false) {
    const active = els.tabs.querySelector('button[aria-pressed="true"]');
    if (!active || !active.offsetWidth) return;
    const x = `${active.offsetLeft}px`;
    const w = `${active.offsetWidth}px`;
    const wasReady = els.tabs.classList.contains('seg-ready');
    if (wasReady && els.tabs.style.getPropertyValue('--seg-x') === x && els.tabs.style.getPropertyValue('--seg-w') === w) {
      return;
    }
    if (animate && wasReady && !prefersReducedMotion()) {
      if (!els.tabs.classList.contains('seg-animated')) {
        els.tabs.classList.add('seg-animated');
        void els.tabs.offsetWidth;
      }
    } else {
      els.tabs.classList.remove('seg-animated');
    }
    els.tabs.style.setProperty('--seg-x', x);
    els.tabs.style.setProperty('--seg-w', w);
    els.tabs.classList.add('seg-ready');
  }

  function renderAppearance({ animateTabSwitch = false, prevKind = state.kind, animateChoices = false } = {}) {
    els.studio.style.setProperty('--avatar-color', state.color);
    // Preview the accent the Bot will get: Create, focus rings and selections follow the chosen color.
    dialog.style.setProperty('--bot-accent', state.color);
    for (const tab of els.tabs.querySelectorAll('button[data-kind]')) {
      tab.setAttribute('aria-pressed', String(tab.dataset.kind === state.kind));
    }
    syncTabsIndicator(animateTabSwitch);
    renderGallery({ animateChoices });
    renderPreviews();
    renderTextPreview();

    const activeHex = state.color.toLowerCase();
    for (const swatch of els.colors.querySelectorAll('button[data-color]')) {
      const selected = swatch.dataset.color.toLowerCase() === activeHex;
      swatch.setAttribute('aria-pressed', String(selected));
    }
    els.colorField.hidden = state.kind === 'upload';
    els.uploadPanel.hidden = state.kind !== 'upload';
    els.shuffle.disabled = state.kind === 'upload';
    const hasImage = !!currentPreviewImageUrl();
    els.removeUpload.hidden = !hasImage;
    els.uploadLabel.textContent = hasImage ? (state.filename || 'Uploaded image') : 'Choose an image';

    if (animateTabSwitch) {
      if (state.kind === 'upload') {
        animatePanelEnter(els.uploadPanel, 220);
      } else {
        animatePanelEnter(els.gallery, 200);
        if (prevKind === 'upload') animatePanelEnter(els.colorField, 220);
      }
    }
  }

  function setStep(next, { animate = true } = {}) {
    const prevStep = step;
    step = next;
    els.start.hidden = step !== 'start';
    els.studio.hidden = step !== 'editor';
    dialog.setAttribute('aria-labelledby', step === 'start' ? 'studio-start-title' : 'studio-title');
    setSubmitError('');
    if (step === 'start') {
      dialog.style.removeProperty('--bot-accent');
      els.tabs.classList.remove('seg-animated');
      destroyHero();
      onDraft?.(null);
      if (animate && prevStep === 'editor') {
        runAnimation(
          els.start,
          [
            { opacity: 0, transform: 'translateX(-18px)' },
            { opacity: 1, transform: 'translateX(0)' },
          ],
          { duration: 220, easing: EASE_OUT_EXPO },
        );
      }
      const chosen = els.start.querySelector(`[data-preset="${state.role}"]`) ?? els.start.querySelector('[data-preset]');
      chosen.focus({ preventScroll: true });
      return;
    }
    // Keep the chosen preset in view on the appearance step, so it's clear what is being created.
    const preset = PRESETS.find((entry) => entry.id === state.role);
    els.title.textContent = mode === 'edit' ? 'Edit appearance' : preset ? `New ${preset.title} Bot` : 'New Bot';
    els.cancel.textContent = mode === 'edit' ? 'Cancel' : 'Back';
    els.nameInput.value = state.name;
    const enteringFromStart = animate && prevStep === 'start';
    renderAppearance({ animateChoices: enteringFromStart });
    if (enteringFromStart) {
      runAnimation(
        els.studio,
        [
          { opacity: 0, transform: 'translateX(18px)' },
          { opacity: 1, transform: 'translateX(0)' },
        ],
        { duration: 220, easing: EASE_OUT_EXPO },
      );
    }
    els.nameInput.focus({ preventScroll: true });
    if (mode === 'create') els.nameInput.select();
  }

  function handleSelectedFile(file) {
    const generation = ++uploadGeneration;
    if (!file) return;
    if (!ALLOWED_MIME.test(file.type) || file.size > MAX_UPLOAD_BYTES) {
      setUploadError('Choose a PNG, JPEG, WebP or GIF up to 5 MB.');
      els.photo.value = '';
      return;
    }
    const candidateUrl = URL.createObjectURL(file);
    const img = new Image();
    img.onerror = () => {
      URL.revokeObjectURL(candidateUrl);
      if (generation === uploadGeneration) {
        setUploadError('This image could not be opened.');
        els.photo.value = '';
      }
    };
    img.onload = () => {
      if (generation !== uploadGeneration) {
        URL.revokeObjectURL(candidateUrl);
        return;
      }
      revokeObjectUrl();
      objectUrl = candidateUrl;
      state.uploadFile = file;
      state.uploadPreviewUrl = candidateUrl;
      state.filename = file.name;
      state.lookChanged = true;
      setUploadError('');
      setSubmitError('');
      renderAppearance();
      popHeroAvatar();
    };
    img.src = candidateUrl;
  }

  async function saveProfile() {
    if (saving || els.save.disabled) return;
    const trimmedName = state.name.trim();
    if (!trimmedName) return;
    saving = true;
    setSubmitError('');
    els.save.disabled = true;
    els.save.textContent = saveLabel();

    // Snapshot the request before any await so it always targets the Bot this editor opened.
    const target = { mode, botId: state.botId, role: state.role, look: { ...state } };
    const destination = workspace.destination();
    try {
      let avatar;
      const { look } = target;
      if (target.mode === 'create' || look.lookChanged) {
        avatar = { kind: look.kind, color: look.color };
        if (look.kind === 'shape') {
          avatar.example = look.shape;
        } else if (look.kind === 'icon') {
          avatar.icon = look.icon;
        } else if (look.kind === 'upload') {
          if (look.filename) avatar.filename = look.filename;
          if (look.uploadFile) {
            avatar.imageData = await readFileAsBase64(look.uploadFile);
          } else if (!look.existingImageUrl) {
            throw new Error('Choose an image before saving.');
          }
        }
      }

      const savedBot = target.mode === 'edit'
        ? await api('bots/profile', { botId: target.botId, name: trimmedName, ...(avatar && { avatar }) })
        : await api('bots', { name: trimmedName, role: target.role, avatar, ...destination });

      closeReason = 'saved';
      saving = false;
      dialog.close();
      await onSaved?.(savedBot, target.mode);
    } catch (error) {
      const messages = {
        invalid_name: 'Enter a name between 1 and 32 characters.',
        invalid_image: 'Choose a PNG, JPEG, WebP or GIF up to 5 MB.',
        invalid_avatar: 'Check the selected avatar options and try again.',
        invalid_workspace: 'Choose an available T3 environment and project.',
        workspaces_unavailable: 'Couldn’t load the selected project. Check the T3 connection and try again.',
        bot_not_found: 'This Bot no longer exists.',
        bot_management_unavailable: 'Bot editing is unavailable on this server.',
        access_denied: 'You do not have permission to edit this Bot.',
      };
      setSubmitError(messages[error.message] ?? 'Could not save this Bot. Check the runtime connection and try again.');
    } finally {
      saving = false;
      if (dialog.open) renderTextPreview();
    }
  }

  function arrowNavigate(container, selector, event) {
    const buttons = [...container.querySelectorAll(selector)];
    const index = buttons.indexOf(document.activeElement);
    if (index < 0 || buttons.length < 2) return;
    const moves = {
      ArrowRight: index + 1,
      ArrowDown: index + 1,
      ArrowLeft: index - 1,
      ArrowUp: index - 1,
      Home: 0,
      End: buttons.length - 1,
    };
    if (!(event.key in moves)) return;
    event.preventDefault();
    const target = buttons[(moves[event.key] + buttons.length) % buttons.length];
    target.focus();
  }

  els.start.addEventListener('click', (event) => {
    const preset = event.target.closest('button[data-preset]')?.dataset.preset;
    if (!preset) return;
    state.role = preset;
    setStep('editor');
  });
  els.start.addEventListener('keydown', (event) => arrowNavigate(els.start, 'button[data-preset]', event));
  els.cancel.addEventListener('click', () => {
    if (saving) return;
    if (mode === 'create') setStep('start');
    else dialog.close();
  });
  for (const btn of dialog.querySelectorAll('[data-close]')) btn.addEventListener('click', () => { if (!saving) dialog.close(); });
  dialog.addEventListener('cancel', (event) => { if (saving) event.preventDefault(); });
  enableBackdropDismiss(dialog, { canDismiss: () => !saving });

  els.nameInput.addEventListener('input', () => {
    state.name = els.nameInput.value;
    setSubmitError('');
    renderTextPreview();
  });

  els.randomName.addEventListener('click', () => {
    state.name = pickRandomName(state.name, existingNames);
    els.nameInput.value = state.name;
    setSubmitError('');
    renderTextPreview();
    animateDiceIcon();
  });

  els.tabs.addEventListener('click', (event) => {
    const kind = event.target.closest('button[data-kind]')?.dataset.kind;
    if (!kind || kind === state.kind) return;
    const prevKind = state.kind;
    state.kind = kind;
    state.lookChanged = true;
    setSubmitError('');
    renderAppearance({ animateTabSwitch: true, prevKind });
    popHeroAvatar();
  });
  els.tabs.addEventListener('keydown', (event) => arrowNavigate(els.tabs, 'button[data-kind]', event));

  els.gallery.addEventListener('click', (event) => {
    const btn = event.target.closest('button[data-value]');
    if (!btn) return;
    if (state.kind === 'shape') state.shape = btn.dataset.value;
    else if (state.kind === 'icon') state.icon = btn.dataset.value;
    state.lookChanged = true;
    setSubmitError('');
    renderAppearance();
    popHeroAvatar();
  });
  els.gallery.addEventListener('keydown', (event) => arrowNavigate(els.gallery, 'button[data-value]', event));

  els.colors.addEventListener('click', (event) => {
    const swatch = event.target.closest('button[data-color]');
    if (!swatch) return;
    state.color = swatch.dataset.color;
    state.lookChanged = true;
    setSubmitError('');
    renderAppearance();
    popHeroAvatar();
  });
  els.colors.addEventListener('keydown', (event) => arrowNavigate(els.colors, 'button[data-color]', event));

  els.shuffle.addEventListener('click', () => {
    if (state.kind === 'shape') {
      state.shape = pickDifferent(AVATAR_SHAPES.map(([id]) => id), state.shape);
    } else if (state.kind === 'icon') {
      state.icon = pickDifferent(AVATAR_ICONS.map(([id]) => id), state.icon);
    } else {
      return;
    }
    state.color = pickDifferent(AVATAR_COLORS.map(([, hex]) => hex), state.color);
    state.lookChanged = true;
    setSubmitError('');
    renderAppearance();
    animateShuffleIcon();
    popHeroAvatar();
  });

  els.photo.addEventListener('change', (event) => {
    handleSelectedFile(event.target.files?.[0]);
  });

  els.drop.addEventListener('dragover', (event) => {
    event.preventDefault();
    els.drop.classList.add('dragover');
  });
  els.drop.addEventListener('dragleave', () => {
    els.drop.classList.remove('dragover');
  });
  els.drop.addEventListener('drop', (event) => {
    event.preventDefault();
    els.drop.classList.remove('dragover');
    handleSelectedFile(event.dataTransfer?.files?.[0]);
  });

  els.removeUpload.addEventListener('click', () => {
    uploadGeneration++;
    revokeObjectUrl();
    const prevKind = state.kind;
    state.uploadFile = null;
    state.uploadPreviewUrl = '';
    state.existingImageUrl = '';
    state.filename = '';
    // Without an image the avatar falls back to the shape, so show and save that.
    state.kind = 'shape';
    state.lookChanged = true;
    els.photo.value = '';
    setUploadError('');
    setSubmitError('');
    renderAppearance({ animateTabSwitch: prevKind !== 'shape', prevKind });
    popHeroAvatar();
  });

  els.save.addEventListener('click', () => void saveProfile());

  window.addEventListener('resize', () => {
    if (dialog.open && step === 'editor') syncTabsIndicator(false);
  });

  dialog.addEventListener('close', () => {
    uploadGeneration++;
    els.tabs.classList.remove('seg-animated');
    destroyHero();
    revokeObjectUrl();
    els.photo.value = '';
    setUploadError('');
    setSubmitError('');
    workspace.close();
    const reason = closeReason;
    const savedMode = mode;
    closeReason = 'cancel';
    onDraft?.(null);
    if (reason === 'cancel') onCancel?.(savedMode);
    if (returnFocus?.isConnected && document.activeElement === document.body) {
      returnFocus.focus();
    }
    returnFocus = null;
  });

  function openCreate({ bots = [], origin = document.activeElement } = {}) {
    mode = 'create';
    saving = false;
    closeReason = 'cancel';
    existingNames = bots.map((bot) => bot.name || '');
    returnFocus = origin;
    renderedGalleryKind = '';
    renderedGalleryColor = '';
    revokeObjectUrl();
    els.photo.value = '';
    setUploadError('');
    setSubmitError('');
    state = {
      botId: '',
      name: pickRandomName('', existingNames),
      role: 'custom',
      kind: 'shape',
      shape: 'sprout',
      icon: 'star',
      color: AVATAR_COLORS[4][1],
      existingImageUrl: '',
      uploadPreviewUrl: '',
      uploadFile: null,
      filename: '',
    };
    if (!dialog.open) dialog.showModal();
    setStep('start', { animate: false });
    void workspace.open();
  }

  function openEdit(bot, { bots = [], origin = document.activeElement } = {}) {
    mode = 'edit';
    saving = false;
    closeReason = 'cancel';
    existingNames = bots.filter((item) => item.id !== bot.id).map((item) => item.name || '');
    returnFocus = origin;
    renderedGalleryKind = '';
    renderedGalleryColor = '';
    revokeObjectUrl();
    els.photo.value = '';
    setUploadError('');
    setSubmitError('');
    const resolved = resolveAvatar(bot);
    state = {
      botId: bot.id,
      name: bot.name || bot.id,
      role: bot.role || '',
      kind: resolved.kind === 'upload' && !resolved.imageUrl ? 'shape' : resolved.kind,
      shape: resolved.shape.example || 'sprout',
      icon: resolved.kind === 'icon' ? resolved.icon : 'star',
      color: resolved.color || AVATAR_COLORS[4][1],
      existingImageUrl: resolved.kind === 'upload' ? resolved.imageUrl : '',
      uploadPreviewUrl: '',
      uploadFile: null,
      filename: resolved.kind === 'upload' ? resolved.filename : '',
      originalAvatar: bot.avatar ?? null,
      lookChanged: false,
    };
    workspace.close();
    if (!dialog.open) dialog.showModal();
    setStep('editor', { animate: false });
  }

  return { openCreate, openEdit };
}
