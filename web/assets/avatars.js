import { DEFAULT_COLOR, getExample, renderAvatarSvg, validateAvatar } from '/vendor/open-bot-avatar/index.js';
import { mountAvatar } from '/vendor/open-bot-avatar/dom/index.js';

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

function contrastingEyeColor(color) {
  const body = luminance(color);
  return (body + 0.05) / (luminance(ink) + 0.05) >= (luminance(lightInk) + 0.05) / (body + 0.05) ? ink : lightInk;
}

function avatarOptions(bot) {
  const key = JSON.stringify([bot.id, bot.avatar]);
  if (cached.has(key)) return cached.get(key);
  let hash = 2166136261;
  for (const char of bot.id) hash = Math.imul(hash ^ char.codePointAt(0), 16777619) >>> 0;
  const avatar = typeof bot.avatar === 'string' ? { example: bot.avatar } : bot.avatar;
  const example = getExample(avatar?.example) ?? getExample(defaultShapeOrder[hash % defaultShapeOrder.length]);
  const definition = avatar?.definition;
  let validated;
  if (definition && /^[a-z0-9_-]{1,64}$/i.test(definition.id) && definition.body?.path?.length <= 12000) {
    const result = validateAvatar(definition);
    if (result.ok) validated = result.value;
  }
  const customColor = hex.test(avatar?.color) ? avatar.color : undefined;
  const customEyeColor = hex.test(avatar?.eyeColor) ? avatar.eyeColor : undefined;
  const options = {
    definition: validated ?? example,
    color: customColor ?? (validated ? DEFAULT_COLOR : galleryColors[example.id]),
    eyeColor: customEyeColor ?? (customColor ? contrastingEyeColor(customColor) : undefined),
    reducedMotion: 'system',
    label: '',
  };
  cached.set(key, options);
  return options;
}

export function showAvatar(host, bot, { still = true, state = 'idle' } = {}) {
  const { definition, color, eyeColor, ...options } = avatarOptions(bot);
  host.setAttribute('aria-hidden', 'true');
  host.classList.toggle('mug-outline', definition.id === 'mug' && color === galleryColors.mug);
  if (still) {
    host.innerHTML = renderAvatarSvg(definition, { color, eyeColor, state, reducedMotion: true });
    return;
  }
  return mountAvatar(host, { ...options, definition, color, eyeColor, state });
}

export function accentColor(bot) { return avatarOptions(bot).color; }
