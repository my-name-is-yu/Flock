import {
  AVATAR_COLORS,
  AVATAR_ICONS,
  AVATAR_SHAPES,
  contrastingEyeColor,
  resolveAvatar,
  showAvatar,
} from '/avatars.js';

const ROLES = [
  { id: 'reviewer', title: 'Reviewer', detail: 'Check changes and verify results', icon: 'shield-check' },
  { id: 'builder', title: 'Builder', detail: 'Implement changes and run tests', icon: 'wrench' },
  { id: 'researcher', title: 'Researcher', detail: 'Investigate questions and gather evidence', icon: 'telescope' },
  { id: 'custom', title: 'Start from scratch', detail: 'Give your bot a role later', icon: 'plus' },
];

const RANDOM_NAMES = [
  'Moss', 'Wren', 'Juniper', 'Pip', 'Kite', 'Clover', 'Orbit', 'Fern',
  'Nimbus', 'Sol', 'Pebble', 'Scout', 'Fig', 'Ollie', 'Pogo', 'Nova',
];

const MAX_UPLOAD_BYTES = 5 * 1024 * 1024;
const ALLOWED_MIME = /^image\/(png|jpeg|webp|gif)$/;

const UI_ICONS = {
  'arrow-left': '<path d="m12 19-7-7 7-7"/><path d="M19 12H5"/>',
  'arrow-right': '<path d="M5 12h14"/><path d="m12 5 7 7-7 7"/>',
  check: '<path d="M20 6 9 17l-5-5"/>',
  close: '<path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/>',
  plus: '<path d="M5 12h14"/><path d="M12 5v14"/>',
  'shield-check': '<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"/><path d="m9 12 2 2 4-4"/>',
  shuffle: '<path d="m18 14 4 4-4 4"/><path d="m18 2 4 4-4 4"/><path d="M2 18h1.973a4 4 0 0 0 3.3-1.7l5.454-8.6a4 4 0 0 1 3.3-1.7H22"/><path d="M2 6h1.972a4 4 0 0 1 3.6 2.2"/><path d="M22 18h-6.041a4 4 0 0 1-3.3-1.8l-.359-.45"/>',
  sparkles: '<path d="M11.017 2.814a1 1 0 0 1 1.966 0l1.051 5.558a2 2 0 0 0 1.594 1.594l5.558 1.051a1 1 0 0 1 0 1.966l-5.558 1.051a2 2 0 0 0-1.594 1.594l-1.051 5.558a1 1 0 0 1-1.966 0l-1.051-5.558a2 2 0 0 0-1.594-1.594l-5.558-1.051a1 1 0 0 1 0-1.966l5.558-1.051a2 2 0 0 0 1.594-1.594z"/><path d="M20 2v4"/><path d="M22 4h-4"/><circle cx="4" cy="20" r="2"/>',
  telescope: '<path d="m10.065 12.493-6.18 1.318a.934.934 0 0 1-1.108-.702l-.537-2.15a1.07 1.07 0 0 1 .691-1.265l13.504-4.44"/><path d="m13.56 11.747 4.332-.924"/><path d="m16 21-3.105-6.21"/><path d="M16.485 5.94a2 2 0 0 1 1.455-2.425l1.09-.272a1 1 0 0 1 1.212.727l1.515 6.06a1 1 0 0 1-.727 1.213l-1.09.272a2 2 0 0 1-2.425-1.455z"/><path d="m6.158 8.633 1.114 4.456"/><path d="m8 21 3.105-6.21"/><circle cx="12" cy="13" r="2"/>',
  upload: '<path d="M12 3v12"/><path d="m17 8-5-5-5 5"/><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>',
  wrench: '<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.106-3.105c.32-.322.863-.22.983.218a6 6 0 0 1-8.259 7.057l-7.91 7.91a1 1 0 0 1-2.999-3l7.91-7.91a6 6 0 0 1 7.057-8.259c.438.12.54.662.219.984z"/>',
};

const BRAND_MARK = '<svg aria-hidden="true" viewBox="0 0 48 48"><rect width="48" height="48" rx="14" fill="#30264b"/><path d="M11 26c5-.7 8.5.4 11 3.9C24.8 23 29.7 19.5 37 19c-3.3 2.3-6.1 5.2-7.7 8.5-1.9 3.9-4.1 6-7.2 6-4.1 0-5.8-4.1-11.1-7.5Z" fill="#e3d5ff"/><path d="M13 19c4.8-.8 8-.3 10.2 1.6 1.2-3.3 3.9-5.9 8-7.1-3.1 2.8-4.7 6.2-7.3 10.6-2.7-2.9-6-4.1-10.9-5.1Z" fill="#aa92e5"/></svg>';

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

function colorNameFor(hex) {
  const normalized = String(hex ?? '').toLowerCase();
  const match = AVATAR_COLORS.find(([, value]) => value.toLowerCase() === normalized);
  return match ? match[0] : normalized.toUpperCase();
}

function roleLabelFor(roleId, fallbackSoul = '') {
  const found = ROLES.find((role) => role.id === roleId);
  if (found) return found.id === 'custom' ? 'Custom bot' : found.title;
  const line = String(fallbackSoul ?? '').split(/\r?\n/).map((s) => s.trim()).find(Boolean);
  if (line) return line.length > 42 ? `${line.slice(0, 41)}…` : line;
  return 'Bot';
}

export function createBotEditor(dialog, { api, onSaved, onCancel }) {
  dialog.innerHTML = `
    <div class="studio">
      <header class="studio-top">
        <div class="studio-brand">
          <span class="studio-brand-mark">${BRAND_MARK}</span>
          <strong>Flock</strong>
          <span class="studio-divider" aria-hidden="true"></span>
          <span id="studio-mode-label" class="studio-mode-label">New bot</span>
        </div>
        <div class="studio-top-right">
          <nav class="studio-steps" id="studio-steps" aria-label="Bot setup">
            <button type="button" data-step="role" id="studio-role-step">${uiIcon('check')}<span>Role</span></button>
            <span class="studio-step-line" aria-hidden="true"></span>
            <button type="button" data-step="appearance" id="studio-appearance-step">Appearance</button>
          </nav>
          <button type="button" id="studio-close" class="icon-button studio-close" aria-label="Close">${uiIcon('close')}</button>
        </div>
      </header>

      <div class="studio-content">
        <section id="studio-role-panel" hidden>
          <div class="studio-heading">
            <h1 id="studio-role-heading" tabindex="-1">Choose a role</h1>
            <p>Pick a starting point for your bot.</p>
          </div>
          <div id="studio-roles" class="studio-roles" role="group" aria-label="Role templates"></div>
          <footer class="studio-footer">
            <button type="button" class="studio-secondary" id="studio-role-cancel">Cancel</button>
            <button type="button" class="studio-primary" id="studio-continue"><span>Continue</span>${uiIcon('arrow-right')}</button>
          </footer>
        </section>

        <section id="studio-appearance-panel" hidden>
          <div class="studio-heading">
            <h1 id="studio-appearance-heading" tabindex="-1">Customize your bot</h1>
          </div>
          <div class="studio-editor">
            <div class="studio-preview">
              <div class="studio-stage" id="studio-stage">
                <span class="studio-preview-label">Live preview</span>
                <div class="studio-hero-avatar" id="studio-hero-avatar"></div>
                <div class="studio-hero-name" id="studio-hero-name"></div>
                <span class="studio-hero-role" id="studio-hero-role"></span>
              </div>
              <div class="studio-aside-preview">
                <div class="studio-aside-label">In the sidebar</div>
                <div class="studio-bot-row">
                  <div class="studio-thumb avatar" id="studio-side-avatar"></div>
                  <div class="studio-row-copy">
                    <strong id="studio-side-name"></strong>
                    <span id="studio-side-role"></span>
                  </div>
                </div>
              </div>
            </div>

            <div class="studio-options">
              <div class="studio-name-head">
                <label class="studio-field" for="studio-bot-name">Name</label>
                <button type="button" id="studio-random-name" class="studio-subtle">${uiIcon('sparkles')}<span>Random name</span></button>
              </div>
              <input class="studio-name-input" id="studio-bot-name" type="text" maxlength="32" autocomplete="off" placeholder="Bot name">

              <div class="studio-section-head">
                <span class="studio-field">Avatar</span>
                <button type="button" id="studio-shuffle" class="studio-subtle">${uiIcon('shuffle')}<span>Shuffle</span></button>
              </div>
              <div class="studio-tabs" id="studio-tabs" role="group" aria-label="Avatar source">
                <button type="button" data-kind="shape" aria-pressed="true">Shapes</button>
                <button type="button" data-kind="icon" aria-pressed="false">Icons</button>
                <button type="button" data-kind="upload" aria-pressed="false">Upload</button>
              </div>
              <div class="studio-gallery" id="studio-gallery" role="group" aria-label="Avatar choices"></div>
              <div class="studio-upload" id="studio-upload-panel" hidden>
                <label class="studio-drop" id="studio-drop" for="studio-photo">
                  ${uiIcon('upload')}
                  <strong id="studio-upload-label">Choose an image</strong>
                  <span>PNG, JPEG, WebP or GIF · Up to 5 MB</span>
                  <input id="studio-photo" type="file" accept="image/png,image/jpeg,image/webp,image/gif">
                </label>
                <button class="studio-subtle studio-remove-upload" id="studio-remove-upload" type="button" hidden>Remove image</button>
                <p class="studio-error" id="studio-upload-error" role="alert" hidden></p>
              </div>
              <div id="studio-color-field">
                <div class="studio-palette-heading">
                  <span class="studio-field">Color</span>
                  <span id="studio-color-name">Mint</span>
                </div>
                <div id="studio-colors" class="studio-palette" role="group" aria-label="Avatar color"></div>
              </div>
            </div>
          </div>

          <p class="studio-error studio-submit-error" id="studio-submit-error" role="alert" hidden></p>
          <footer class="studio-footer">
            <button type="button" class="studio-secondary" id="studio-back">${uiIcon('arrow-left')}<span>Back</span></button>
            <button type="button" class="studio-primary" id="studio-save"><span>Create bot</span>${uiIcon('arrow-right')}</button>
          </footer>
        </section>
      </div>
    </div>
  `;

  const el = (id) => dialog.querySelector(`#${id}`);
  const els = {
    studio: dialog.querySelector('.studio'),
    modeLabel: el('studio-mode-label'),
    steps: el('studio-steps'),
    roleStep: el('studio-role-step'),
    appearanceStep: el('studio-appearance-step'),
    close: el('studio-close'),
    rolePanel: el('studio-role-panel'),
    roleHeading: el('studio-role-heading'),
    roles: el('studio-roles'),
    roleCancel: el('studio-role-cancel'),
    continueBtn: el('studio-continue'),
    appearancePanel: el('studio-appearance-panel'),
    appearanceHeading: el('studio-appearance-heading'),
    heroAvatar: el('studio-hero-avatar'),
    heroName: el('studio-hero-name'),
    heroRole: el('studio-hero-role'),
    sideAvatar: el('studio-side-avatar'),
    sideName: el('studio-side-name'),
    sideRole: el('studio-side-role'),
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
    colorName: el('studio-color-name'),
    colors: el('studio-colors'),
    submitError: el('studio-submit-error'),
    back: el('studio-back'),
    save: el('studio-save'),
  };

  for (const role of ROLES) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'studio-role-option';
    btn.dataset.role = role.id;
    btn.innerHTML = `<span class="studio-role-icon">${uiIcon(role.icon)}</span><span><strong>${role.title}</strong><small>${role.detail}</small></span>`;
    els.roles.append(btn);
  }

  for (const [label, hex] of AVATAR_COLORS) {
    const swatch = document.createElement('button');
    swatch.type = 'button';
    swatch.className = 'studio-swatch';
    swatch.dataset.color = hex;
    swatch.title = label;
    swatch.setAttribute('aria-label', label);
    swatch.style.setProperty('--swatch', hex);
    swatch.style.setProperty('--swatch-ink', contrastingEyeColor(hex));
    els.colors.append(swatch);
  }

  let mode = 'create';
  let step = 'role';
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
    heroHandle?.destroy();
    heroHandle = null;
    heroRenderedKind = '';
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

  function renderTextPreview() {
    const displayName = state.name.trim() || 'Untitled bot';
    const roleText = roleLabelFor(state.role, state.soul);
    els.heroName.textContent = displayName;
    els.sideName.textContent = displayName;
    els.heroRole.textContent = roleText;
    els.sideRole.textContent = roleText;
    const hasValidImage = state.kind !== 'upload' || !!currentPreviewImageUrl();
    els.save.disabled = saving || !state.name.trim() || !hasValidImage;
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
    } else {
      destroyHero();
      showAvatar(els.heroAvatar, previewBot(avatar), { still: true });
      heroRenderedKind = avatar.kind;
    }
    showAvatar(els.sideAvatar, previewBot(avatar), { still: true });
  }

  function renderGallery() {
    const kind = state.kind;
    if (renderedGalleryKind !== kind) {
      els.gallery.replaceChildren();
      const entries = kind === 'shape' ? AVATAR_SHAPES : AVATAR_ICONS;
      if (kind !== 'upload') {
        for (const [value, label] of entries) {
          const btn = document.createElement('button');
          btn.type = 'button';
          btn.className = 'studio-choice';
          btn.dataset.value = value;
          const mini = document.createElement('span');
          mini.className = 'studio-mini';
          mini.setAttribute('aria-hidden', 'true');
          const text = document.createElement('span');
          text.textContent = label;
          btn.append(mini, text);
          els.gallery.append(btn);
        }
      }
    }
    for (const btn of els.gallery.querySelectorAll('button')) {
      const value = btn.dataset.value;
      const selected = (kind === 'shape' ? state.shape : state.icon) === value;
      btn.setAttribute('aria-pressed', String(selected));
      if (renderedGalleryKind !== kind || renderedGalleryColor !== state.color) {
        const optionAvatar = kind === 'shape'
          ? { kind: 'shape', example: value, color: state.color }
          : { kind: 'icon', icon: value, color: state.color };
        showAvatar(btn.firstElementChild, previewBot(optionAvatar), { still: true });
      }
    }
    renderedGalleryKind = kind;
    renderedGalleryColor = state.color;
    els.gallery.hidden = kind === 'upload';
  }

  function renderAppearance() {
    els.studio.style.setProperty('--avatar-color', state.color);
    for (const tab of els.tabs.querySelectorAll('button[data-kind]')) {
      tab.setAttribute('aria-pressed', String(tab.dataset.kind === state.kind));
    }
    renderGallery();
    renderPreviews();
    renderTextPreview();

    const activeHex = state.color.toLowerCase();
    for (const swatch of els.colors.querySelectorAll('button[data-color]')) {
      const selected = swatch.dataset.color.toLowerCase() === activeHex;
      swatch.setAttribute('aria-pressed', String(selected));
      swatch.innerHTML = selected ? uiIcon('check') : '';
    }
    els.colorName.textContent = colorNameFor(state.color);
    els.colorField.hidden = state.kind === 'upload';
    els.uploadPanel.hidden = state.kind !== 'upload';
    els.shuffle.hidden = state.kind === 'upload';
    const hasImage = !!currentPreviewImageUrl();
    els.removeUpload.hidden = !hasImage;
    els.uploadLabel.textContent = hasImage ? (state.filename || 'Uploaded image') : 'Choose an image';
  }

  function renderRoles() {
    for (const btn of els.roles.querySelectorAll('button[data-role]')) {
      btn.setAttribute('aria-pressed', String(btn.dataset.role === state.role));
    }
  }

  function setStep(nextStep, moveFocus = true) {
    step = nextStep;
    els.rolePanel.hidden = step !== 'role';
    els.appearancePanel.hidden = step !== 'appearance';
    els.steps.hidden = mode === 'edit';
    els.modeLabel.textContent = mode === 'edit' ? 'Edit bot' : 'New bot';
    dialog.setAttribute('aria-labelledby', step === 'role' ? 'studio-role-heading' : 'studio-appearance-heading');

    for (const btn of els.steps.querySelectorAll('button[data-step]')) {
      if (btn.dataset.step === step) btn.setAttribute('aria-current', 'step');
      else btn.removeAttribute('aria-current');
    }
    els.roleStep.firstElementChild.toggleAttribute('hidden', step === 'role');
    setSubmitError('');

    if (step === 'role') {
      destroyHero();
      renderRoles();
    } else {
      els.appearanceHeading.textContent = mode === 'edit' ? 'Edit appearance' : 'Customize your bot';
      els.nameInput.value = state.name;
      els.back.querySelector('span').textContent = mode === 'edit' ? 'Cancel' : 'Back';
      els.back.firstElementChild.toggleAttribute('hidden', mode === 'edit');
      els.save.querySelector('span').textContent = saving
        ? (mode === 'edit' ? 'Saving…' : 'Creating…')
        : (mode === 'edit' ? 'Save changes' : 'Create bot');
      renderAppearance();
    }

    if (moveFocus) {
      const heading = step === 'role' ? els.roleHeading : els.appearanceHeading;
      heading.focus({ preventScroll: true });
    }
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
      setUploadError('');
      setSubmitError('');
      renderAppearance();
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
    els.save.querySelector('span').textContent = mode === 'edit' ? 'Saving…' : 'Creating…';

    try {
      const avatar = { kind: state.kind, color: state.color };
      if (state.kind === 'shape') {
        avatar.example = state.shape;
      } else if (state.kind === 'icon') {
        avatar.icon = state.icon;
      } else if (state.kind === 'upload') {
        if (state.filename) avatar.filename = state.filename;
        if (state.uploadFile) {
          avatar.imageData = await readFileAsBase64(state.uploadFile);
        } else if (!state.existingImageUrl) {
          throw new Error('Choose an image before saving.');
        }
      }

      const savedBot = mode === 'edit'
        ? await api('bots/profile', { botId: state.botId, name: trimmedName, avatar })
        : await api('bots', { name: trimmedName, role: state.role, avatar });

      closeReason = 'saved';
      dialog.close();
      await onSaved?.(savedBot, mode);
    } catch (error) {
      const messages = {
        invalid_name: 'Enter a name between 1 and 32 characters.',
        invalid_image: 'Choose a PNG, JPEG, WebP or GIF up to 5 MB.',
        invalid_avatar: 'Check the selected avatar options and try again.',
        bot_not_found: 'This Bot no longer exists.',
        bot_management_unavailable: 'Bot editing is unavailable on this server.',
      };
      setSubmitError(messages[error.message] ?? 'Could not save this Bot. Check the runtime connection and try again.');
    } finally {
      saving = false;
      if (dialog.open) {
        els.save.querySelector('span').textContent = mode === 'edit' ? 'Save changes' : 'Create bot';
        renderTextPreview();
      }
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

  els.roles.addEventListener('click', (event) => {
    const btn = event.target.closest('button[data-role]');
    if (!btn) return;
    state.role = btn.dataset.role;
    renderRoles();
  });
  els.roles.addEventListener('keydown', (event) => arrowNavigate(els.roles, 'button[data-role]', event));

  els.continueBtn.addEventListener('click', () => setStep('appearance'));
  els.roleCancel.addEventListener('click', () => dialog.close());
  els.close.addEventListener('click', () => dialog.close());

  els.steps.addEventListener('click', (event) => {
    const target = event.target.closest('button[data-step]')?.dataset.step;
    if (target && target !== step) setStep(target);
  });

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
  });

  els.tabs.addEventListener('click', (event) => {
    const kind = event.target.closest('button[data-kind]')?.dataset.kind;
    if (!kind || kind === state.kind) return;
    state.kind = kind;
    setSubmitError('');
    renderAppearance();
  });
  els.tabs.addEventListener('keydown', (event) => arrowNavigate(els.tabs, 'button[data-kind]', event));

  els.gallery.addEventListener('click', (event) => {
    const btn = event.target.closest('button[data-value]');
    if (!btn) return;
    if (state.kind === 'shape') state.shape = btn.dataset.value;
    else if (state.kind === 'icon') state.icon = btn.dataset.value;
    setSubmitError('');
    renderAppearance();
  });
  els.gallery.addEventListener('keydown', (event) => arrowNavigate(els.gallery, 'button[data-value]', event));

  els.colors.addEventListener('click', (event) => {
    const swatch = event.target.closest('button[data-color]');
    if (!swatch) return;
    state.color = swatch.dataset.color;
    setSubmitError('');
    renderAppearance();
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
    setSubmitError('');
    renderAppearance();
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
    state.uploadFile = null;
    state.uploadPreviewUrl = '';
    state.existingImageUrl = '';
    state.filename = '';
    els.photo.value = '';
    setUploadError('');
    setSubmitError('');
    renderAppearance();
  });

  els.back.addEventListener('click', () => {
    if (mode === 'edit') dialog.close();
    else setStep('role');
  });

  els.save.addEventListener('click', () => void saveProfile());

  dialog.addEventListener('close', () => {
    uploadGeneration++;
    destroyHero();
    revokeObjectUrl();
    els.photo.value = '';
    setUploadError('');
    setSubmitError('');
    const reason = closeReason;
    const savedMode = mode;
    closeReason = 'cancel';
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
      role: 'reviewer',
      soul: '',
      kind: 'shape',
      shape: 'sprout',
      icon: 'star',
      color: AVATAR_COLORS[4][1],
      existingImageUrl: '',
      uploadPreviewUrl: '',
      uploadFile: null,
      filename: '',
    };
    setStep('role', false);
    if (!dialog.open) dialog.showModal();
    els.roleHeading.focus({ preventScroll: true });
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
      soul: bot.soul || '',
      kind: resolved.kind === 'upload' && !resolved.imageUrl ? 'shape' : resolved.kind,
      shape: resolved.shape.example || 'sprout',
      icon: resolved.kind === 'icon' ? resolved.icon : 'star',
      color: resolved.color || AVATAR_COLORS[4][1],
      existingImageUrl: resolved.kind === 'upload' ? resolved.imageUrl : '',
      uploadPreviewUrl: '',
      uploadFile: null,
      filename: resolved.kind === 'upload' ? resolved.filename : '',
    };
    setStep('appearance', false);
    if (!dialog.open) dialog.showModal();
    els.appearanceHeading.focus({ preventScroll: true });
  }

  return { openCreate, openEdit };
}
