import { DEFAULT_COLOR, getExample, renderAvatarSvg, validateAvatar } from '/vendor/open-bot-avatar/index.js';
import { mountAvatar } from '/vendor/open-bot-avatar/dom/index.js';

export const AVATAR_SHAPES = [
  ['mug', 'Mug'],
  ['sprout', 'Sprout'],
  ['controller', 'Controller'],
  ['lightbulb', 'Lightbulb'],
  ['chat', 'Chat'],
  ['watering-can', 'Watering can'],
  ['barbell', 'Barbell'],
];

export const AVATAR_ICONS = [
  ['star', 'Star'],
  ['orbit', 'Orbit'],
  ['zap', 'Bolt'],
  ['compass', 'Compass'],
  ['feather', 'Feather'],
  ['hexagon', 'Hexagon'],
  ['telescope', 'Telescope'],
  ['sparkles', 'Sparkle'],
];

export const AVATAR_COLORS = [
  ['Coral', '#ee8d85'],
  ['Apricot', '#efa374'],
  ['Honey', '#eec767'],
  ['Lime', '#acd077'],
  ['Mint', '#69c9ac'],
  ['Sky', '#75b8e8'],
  ['Indigo', '#929dee'],
  ['Lilac', '#b5a3ef'],
  ['Rose', '#e59bc5'],
  ['Pearl', '#e1dfdf'],
];

const avatarIconPaths = {
  star: '<path d="M11.525 2.295a.53.53 0 0 1 .95 0l2.31 4.679a2.123 2.123 0 0 0 1.595 1.16l5.166.756a.53.53 0 0 1 .294.904l-3.736 3.638a2.123 2.123 0 0 0-.611 1.878l.882 5.14a.53.53 0 0 1-.771.56l-4.618-2.428a2.122 2.122 0 0 0-1.973 0L6.396 21.01a.53.53 0 0 1-.77-.56l.881-5.139a2.122 2.122 0 0 0-.611-1.879L2.16 9.795a.53.53 0 0 1 .294-.906l5.165-.755a2.122 2.122 0 0 0 1.597-1.16z"/>',
  orbit: '<path d="M20.341 6.484A10 10 0 0 1 10.266 21.85"/><path d="M3.659 17.516A10 10 0 0 1 13.74 2.152"/><circle cx="12" cy="12" r="3"/><circle cx="19" cy="5" r="2"/><circle cx="5" cy="19" r="2"/>',
  zap: '<path d="M15.914 4a1.5 1.5 0 00-2.474-1.561l-9 9A1.5 1.5 0 005.5 14h4.002a.5.5 0 01.471.666L8.086 20a1.5 1.5 0 002.475 1.56l9-9A1.5 1.5 0 0018.5 10h-3.997a.5.5 0 01-.472-.667z"/>',
  compass: '<circle cx="12" cy="12" r="10"/><path d="m16.24 7.76-1.804 5.411a2 2 0 0 1-1.265 1.265L7.76 16.24l1.804-5.411a2 2 0 0 1 1.265-1.265z"/>',
  feather: '<path d="M14.086 18.412A2 2 0 0112.67 19H5v-7.672a2 2 0 01.586-1.414L11.75 3.75a6 6 0 118.49 8.49z"/><path d="M16 8 2 22"/><path d="M17.488 15H9"/>',
  hexagon: '<path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/>',
  telescope: '<path d="m10.065 12.493-6.18 1.318a.934.934 0 0 1-1.108-.702l-.537-2.15a1.07 1.07 0 0 1 .691-1.265l13.504-4.44"/><path d="m13.56 11.747 4.332-.924"/><path d="m16 21-3.105-6.21"/><path d="M16.485 5.94a2 2 0 0 1 1.455-2.425l1.09-.272a1 1 0 0 1 1.212.727l1.515 6.06a1 1 0 0 1-.727 1.213l-1.09.272a2 2 0 0 1-2.425-1.455z"/><path d="m6.158 8.633 1.114 4.456"/><path d="m8 21 3.105-6.21"/><circle cx="12" cy="13" r="2"/>',
  sparkles: '<path d="M11.017 2.814a1 1 0 0 1 1.966 0l1.051 5.558a2 2 0 0 0 1.594 1.594l5.558 1.051a1 1 0 0 1 0 1.966l-5.558 1.051a2 2 0 0 0-1.594 1.594l-1.051 5.558a1 1 0 0 1-1.966 0l-1.051-5.558a2 2 0 0 0-1.594-1.594l-5.558-1.051a1 1 0 0 1 0-1.966l5.558-1.051a2 2 0 0 0 1.594-1.594z"/><path d="M20 2v4"/><path d="M22 4h-4"/><circle cx="4" cy="20" r="2"/>',
};

const galleryColors = {
  mug: '#f4f4f4',
  sprout: '#1fbf68',
  controller: '#ff6a1a',
  lightbulb: '#f8a01c',
  chat: '#8b5cf6',
  'watering-can': '#14b3a5',
  barbell: '#f2384b',
};
const defaultShapeOrder = Object.keys(galleryColors);
const ink = '#14121c';
const lightInk = '#fafaff';
const hex = /^#(?:[0-9a-f]{3}|[0-9a-f]{6})$/i;
const cached = new Map();

function luminance(color) {
  const digits = color.length === 4 ? [...color.slice(1)].map((d) => d + d) : color.slice(1).match(/../g);
  const [r, g, b] = digits.map((d) => {
    const s = parseInt(d, 16) / 255;
    return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrastingEyeColor(color) {
  const body = luminance(color);
  return (body + 0.05) / (luminance(ink) + 0.05) >= (luminance(lightInk) + 0.05) / (body + 0.05) ? ink : lightInk;
}

export function avatarIconSvg(name) {
  const paths = avatarIconPaths[name] ?? avatarIconPaths.star;
  return `<svg aria-hidden="true" focusable="false" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round">${paths}</svg>`;
}

export function presenceMotion(presence) {
  switch (presence) {
    case 'queued':
      return 'waiting';
    case 'stopping':
      return 'thinking';
    default:
      return presence;
  }
}

function safeImageUrl(url) {
  if (typeof url !== 'string' || !url) return '';
  if (url.startsWith('/api/bot-avatar?') || url.startsWith('blob:')) return url;
  return '';
}

export function resolveAvatar(bot) {
  const id = typeof bot?.id === 'string' ? bot.id : '';
  const avatar = bot?.avatar;
  const key = JSON.stringify([id, avatar]);
  if (cached.has(key)) return cached.get(key);

  let hash = 2166136261;
  for (const char of id) hash = Math.imul(hash ^ char.codePointAt(0), 16777619) >>> 0;
  const example = getExample(avatar?.example) ?? getExample(defaultShapeOrder[hash % defaultShapeOrder.length]);
  const definition = avatar?.definition;
  let validated;
  if (definition && /^[a-z0-9_-]{1,64}$/i.test(definition.id) && definition.body?.path?.length <= 12000) {
    const result = validateAvatar(definition);
    if (result.ok) validated = result.value;
  }
  const customColor = hex.test(avatar?.color) ? avatar.color : undefined;
  const customEyeColor = hex.test(avatar?.eyeColor) ? avatar.eyeColor : undefined;
  const shapeColor = customColor ?? (validated ? DEFAULT_COLOR : galleryColors[example.id]);
  const shapeOptions = {
    definition: validated ?? example,
    example: example.id,
    color: shapeColor,
    eyeColor: customEyeColor ?? (customColor ? contrastingEyeColor(customColor) : undefined),
    reducedMotion: 'system',
    label: '',
  };

  let resolved;
  if (avatar?.kind === 'upload') {
    const imageUrl = safeImageUrl(avatar.imageUrl);
    resolved = {
      kind: 'upload',
      imageUrl,
      filename: typeof avatar.filename === 'string' ? avatar.filename : '',
      color: customColor ?? '#69c9ac',
      shape: shapeOptions,
    };
  } else if (avatar?.kind === 'icon') {
    const icon = Object.hasOwn(avatarIconPaths, avatar.icon) ? avatar.icon : 'star';
    const color = customColor ?? '#69c9ac';
    resolved = {
      kind: 'icon',
      icon,
      color,
      ink: contrastingEyeColor(color),
      shape: shapeOptions,
    };
  } else {
    resolved = {
      kind: 'shape',
      ...shapeOptions,
      shape: shapeOptions,
    };
  }

  if (cached.size > 256) cached.clear();
  cached.set(key, resolved);
  return resolved;
}

export function showAvatar(host, bot, { still = true, state = 'idle', followPointer = false } = {}) {
  const resolved = resolveAvatar(bot);
  host.setAttribute('aria-hidden', 'true');
  host.classList.remove('mug-outline', 'avatar-kind-upload', 'avatar-kind-icon', 'avatar-kind-shape');

  if (resolved.kind === 'upload' && resolved.imageUrl) {
    host.classList.add('avatar-kind-upload');
    const img = document.createElement('img');
    img.className = 'avatar-photo';
    img.alt = '';
    img.src = resolved.imageUrl;
    host.replaceChildren(img);
    return null;
  }

  if (resolved.kind === 'icon') {
    host.classList.add('avatar-kind-icon');
    const badge = document.createElement('span');
    badge.className = 'avatar-icon-glyph';
    badge.style.setProperty('--avatar-color', resolved.color);
    badge.style.setProperty('--avatar-ink', resolved.ink);
    badge.innerHTML = avatarIconSvg(resolved.icon);
    host.replaceChildren(badge);
    return null;
  }

  const shape = resolved.shape;
  host.classList.add('avatar-kind-shape');
  const isPale = luminance(shape.color) > 0.72;
  host.classList.toggle('mug-outline', isPale);
  if (still) {
    host.innerHTML = renderAvatarSvg(shape.definition, {
      color: shape.color,
      eyeColor: shape.eyeColor,
      state,
      reducedMotion: true,
      label: '',
    });
    return null;
  }
  return mountAvatar(host, {
    definition: shape.definition,
    color: shape.color,
    eyeColor: shape.eyeColor,
    state,
    followPointer,
    reducedMotion: 'system',
    label: '',
  });
}

export function accentColor(bot) {
  return resolveAvatar(bot).color;
}
