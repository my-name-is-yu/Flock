import { getExample, renderAvatarSvg, validateAvatar } from '/vendor/open-bot-avatar/index.js';
import { mountAvatar } from '/vendor/open-bot-avatar/dom/index.js';

const shapes = ['mug', 'sprout', 'controller', 'lightbulb', 'chat', 'watering-can', 'barbell'];
const palettes = [
  ['#a581e1', '#ede3fc'], ['#5cae91', '#dbeee4'], ['#de915c', '#f7e6d7'],
  ['#7693d8', '#e3e9f9'], ['#d379a0', '#f5e3ed'], ['#bd9c44', '#f6edce'],
  ['#609fb0', '#deedf1'],
];
const hex = /^#(?:[0-9a-f]{3}|[0-9a-f]{6})$/i;
const cached = new Map();

function avatarOptions(bot) {
  const key = JSON.stringify([bot.id, bot.avatar]);
  if (cached.has(key)) return cached.get(key);
  let hash = 2166136261;
  for (const char of bot.id) hash = Math.imul(hash ^ char.codePointAt(0), 16777619) >>> 0;
  const [color, background] = palettes[Math.floor(hash / shapes.length) % palettes.length];
  const avatar = typeof bot.avatar === 'string' ? { example: bot.avatar } : bot.avatar;
  const example = getExample(avatar?.example) ?? getExample(shapes[hash % shapes.length]);
  const definition = avatar?.definition;
  let validated;
  if (definition && /^[a-z0-9_-]{1,64}$/i.test(definition.id) && definition.body?.path?.length <= 12000) {
    const result = validateAvatar(definition);
    if (result.ok) validated = result.value;
  }
  const options = {
    definition: validated ?? example,
    color: hex.test(avatar?.color) ? avatar.color : color,
    eyeColor: hex.test(avatar?.eyeColor) ? avatar.eyeColor : '#292037',
    background,
    reducedMotion: 'system',
    label: '',
  };
  cached.set(key, options);
  return options;
}

export function showAvatar(host, bot, { still = true, state = 'idle' } = {}) {
  const { background, definition, color, eyeColor, ...options } = avatarOptions(bot);
  host.style.setProperty('--avatar-bg', background);
  host.setAttribute('aria-hidden', 'true');
  if (still) {
    host.innerHTML = renderAvatarSvg(definition, { color, eyeColor, state, reducedMotion: true });
    return;
  }
  return mountAvatar(host, { ...options, definition, color, eyeColor, state });
}
