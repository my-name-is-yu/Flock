import { getExample, renderAvatarSvg, validateAvatar } from '/vendor/open-bot-avatar/index.js';
import { mountAvatar } from '/vendor/open-bot-avatar/dom/index.js';

const shapes = ['mug', 'sprout', 'controller', 'lightbulb', 'chat', 'watering-can', 'barbell'];
// Saturated mid-tones that read on both the light and the dark canvas. A bot's color is also its
// accent (avatarOptions(bot).color), and the conversation fills that Bot's user bubbles with it:
// every color below keeps at least 4.5:1 with dark text (`ink`, or anything darker); white text
// does not reach 4.5:1 on them. A color set in the bot's config carries no such guarantee.
const palette = [
  '#ff2e30', '#e06e00', '#da9f00', '#00ac2e', '#00a790',
  '#00a2c7', '#0083fd', '#7773ff', '#b852ff', '#fe00a9',
];
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

// Eyes for a configured body color: whichever of the two inks contrasts more with it.
function eyesFor(color) {
  const body = luminance(color);
  return (body + 0.05) / (luminance(ink) + 0.05) >= (luminance(lightInk) + 0.05) / (body + 0.05) ? ink : lightInk;
}

function avatarOptions(bot) {
  const key = JSON.stringify([bot.id, bot.avatar]);
  if (cached.has(key)) return cached.get(key);
  let hash = 2166136261;
  for (const char of bot.id) hash = Math.imul(hash ^ char.codePointAt(0), 16777619) >>> 0;
  const avatar = typeof bot.avatar === 'string' ? { example: bot.avatar } : bot.avatar;
  const example = getExample(avatar?.example) ?? getExample(shapes[hash % shapes.length]);
  const definition = avatar?.definition;
  let validated;
  if (definition && /^[a-z0-9_-]{1,64}$/i.test(definition.id) && definition.body?.path?.length <= 12000) {
    const result = validateAvatar(definition);
    if (result.ok) validated = result.value;
  }
  const color = hex.test(avatar?.color) ? avatar.color : palette[Math.floor(hash / shapes.length) % palette.length];
  const options = {
    definition: validated ?? example,
    color,
    eyeColor: hex.test(avatar?.eyeColor) ? avatar.eyeColor : hex.test(avatar?.color) ? eyesFor(color) : ink,
    reducedMotion: 'system',
    label: '',
  };
  cached.set(key, options);
  return options;
}

export function showAvatar(host, bot, { still = true, state = 'idle' } = {}) {
  const { definition, color, eyeColor, ...options } = avatarOptions(bot);
  host.setAttribute('aria-hidden', 'true');
  if (still) {
    host.innerHTML = renderAvatarSvg(definition, { color, eyeColor, state, reducedMotion: true });
    return;
  }
  return mountAvatar(host, { ...options, definition, color, eyeColor, state });
}
