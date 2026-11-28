import { showAvatar } from './avatars.js';
import { waitLabels } from './conversation-copy.mjs';
import { node } from './dom.js';
import {
  bots,
  byId,
  conversationActions,
  conversationActionsTrigger,
  elements,
  motionAnimations,
  motionEaseOut,
  narrow,
  reducedMotion,
  responseViewers,
  signatures,
  view,
  workspace,
} from './app-state.js';

const motionSpring = 'cubic-bezier(0.22, 1.2, 0.36, 1)';
export function cancelMotion(el) {
  const prev = el && motionAnimations.get(el);
  if (!prev) return;
  motionAnimations.delete(el);
  prev.cancel();
}
export function runMotion(el, keyframes, options) {
  if (!el?.animate || reducedMotion.matches) return null;
  cancelMotion(el);
  const animation = el.animate(keyframes, options);
  motionAnimations.set(el, animation);
  const clear = () => {
    if (motionAnimations.get(el) === animation) motionAnimations.delete(el);
  };
  animation.addEventListener('finish', clear, { once: true });
  animation.addEventListener('cancel', clear, { once: true });
  return animation;
}
export function animateLiveReply(id) {
  runMotion(byId(`event-${id}`)?.querySelector('.bubble'), [
    { opacity: .5, transform: 'translateY(8px) scale(.975)' },
    { opacity: 1, transform: 'none' },
  ], { duration: 200, easing: motionSpring });
}
export function animateSentMessage(id) {
  runMotion(byId(`event-${id}`)?.querySelector('.bubble'), [
    { opacity: 0, transform: 'translateY(10px) scale(.96)' },
    { opacity: 1, transform: 'none' },
  ], { duration: 210, easing: motionSpring });
}
export function saved(key, fallback) {
  try { return JSON.parse(localStorage.getItem(key)) ?? fallback; }
  catch { return fallback; }
}
export function save(key, value) {
  try { localStorage.setItem(key, JSON.stringify(value)); }
  catch {}
}
const savedPins = saved('flock-pins', []);
export const pins = new Set(Array.isArray(savedPins) ? savedPins : []);

export function button(text, className, click) {
  const el = node('button', className, text);
  el.type = 'button';
  if (click) el.addEventListener('click', click);
  return el;
}
export function avatar(bot, className = '') {
  const host = node('span', `avatar ${className}`.trim());
  showAvatar(host, bot);
  return host;
}
export function runHref(run) {
  if (!run?.link) return null;
  let url;
  try { url = new URL(run.link); }
  catch { return null; }
  const hosted = url.origin === 'https://app.t3.codes' && url.pathname.startsWith('/threads/');
  const local = url.protocol === 'http:' && ['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname);
  if ((!hosted && !local) || url.username || url.password) return null;
  return url.href;
}
export function linkToRun(run, label = 'Open in T3') {
  const href = runHref(run);
  if (!href) return null;
  const link = node('a', 'link', label);
  link.href = href;
  link.target = '_blank';
  link.rel = 'noopener noreferrer';
  return link;
}
const openBotName = () => view?.data?.bot?.name ?? bots.find((bot) => bot.id === view?.id)?.name ?? 'The Bot';
const withBotName = (text) => text?.replaceAll('$BOT', openBotName);
export function waitLabel(reason) { return withBotName(waitLabels[reason]?.[0]) ?? 'Waiting for you'; }
export function waitText(wait, { spokenByBot = false } = {}) {
  const labels = waitLabels[wait.reason];
  const fallback = withBotName((spokenByBot && labels?.[2]) || labels?.[1]);
  if (wait.reason === 'policy_approval') return fallback;
  return wait.text || fallback || 'Refresh the pending request.';
}
export function quotedText(source) {
  const data = source?.data;
  if (!data) return 'Earlier message';
  if (source.kind === 'waiting') return waitText(data, { spokenByBot: true });
  return data.report || data.text || data.title || 'Earlier message';
}
export function changed(key, ...values) {
  const signature = JSON.stringify(values);
  if (signatures.get(key) === signature) return false;
  signatures.set(key, signature);
  return true;
}
export async function api(path, body, { method = body ? 'POST' : 'GET', signal } = {}) {
  const headers = new Headers();
  if (method !== 'GET') headers.set('X-Flock-Action', '1');
  if (body) headers.set('Content-Type', 'application/json');
  const response = await fetch(`/api/${path}`, {
    method, headers, credentials: 'same-origin', signal, body: body instanceof Blob ? body : body ? JSON.stringify(body) : undefined,
  });
  const data = await response.json();
  if (!response.ok) throw Object.assign(new Error(data.error), { status: response.status, data });
  const viewer = response.headers.get('X-Flock-Viewer');
  if (viewer !== null) responseViewers.set(data, viewer);
  return data;
}
const mainElement = byId('main');
export function openNavigation(open, focus = true) {
  const expanded = open && narrow.matches;
  const navigationVisibilityChanged = workspace.classList.contains('nav-open') !== expanded;
  workspace.classList.toggle('nav-open', expanded);
  if (mainElement) mainElement.inert = expanded;
  elements['open-navigation'].setAttribute('aria-expanded', expanded);
  if (focus && narrow.matches && navigationVisibilityChanged) elements[expanded ? 'close-navigation' : 'open-navigation'].focus();
}
export function closeConversationActions(focus = false) {
  if (!conversationActions.open) return;
  conversationActions.open = false;
  if (focus) conversationActionsTrigger.focus({ preventScroll: true });
}
export function conversationUrl(botId) {
  const url = new URL(location.href);
  if (url.searchParams.get('bot') !== botId) {
    url.searchParams.delete('thread');
    url.searchParams.delete('message');
  }
  if (botId) url.searchParams.set('bot', botId);
  else url.searchParams.delete('bot');
  return url;
}
export function animateTimelineContent(kind) {
  if (reducedMotion.matches) return;
  const inner = elements.conversation.querySelector('.conversation-inner');
  cancelMotion(inner);
  cancelMotion(elements.timeline);
  if (kind === 'bot-switch') {
    runMotion(elements.timeline, [
      { opacity: .15, transform: 'translateY(8px)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 210, easing: motionEaseOut });
  }
}
export function animateViewTransition(kind) {
  const inner = elements.conversation.querySelector('.conversation-inner');
  const profileHeading = byId('profile-heading');
  for (const target of [inner, elements.timeline, elements['profile-avatar-button'], profileHeading]) {
    cancelMotion(target);
  }
  if (reducedMotion.matches) return;
  const context = view;
  if (elements.timeline?.childElementCount > 0) {
    animateTimelineContent(kind);
    if (context) context.pendingViewMotion = null;
  } else if (context) {
    context.pendingViewMotion = kind;
  }
  if (kind === 'bot-switch') {
    runMotion(elements['profile-avatar-button'], [
      { opacity: .6, transform: 'scale(.88) translateY(3px)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 230, easing: motionSpring });
    runMotion(profileHeading, [
      { opacity: .5, transform: 'translateY(4px) scale(.96)' },
      { opacity: 1, transform: 'none' },
    ], { duration: 220, easing: motionSpring });
  }
}
