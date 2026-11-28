import { stageScreenshots } from './composer-screenshots.mjs';
import { node } from './dom.js';
import {
  assignComposition,
  assignDragDepth,
  byteLength,
  closedQuestion,
  composerLimit,
  composition,
  elements,
  notices,
  screenshotAnchor,
  screenshotDrafts,
  screenshotsFor,
  sendingMessages,
  showingComposer,
  view,
} from './app-state.js';
import { button, waitLabel, waitText } from './app-core.js';
import { nextSuggestion, timelineAtEnd } from './timeline.js';
import { headerFocusTarget } from './bot-detail.js';
import { decisions } from './app.js';

export function setNotice(context, text, isError = false, { quiet = false } = {}) {
  notices.set(context.id, { text, isError, quiet });
  if (showingComposer(context.id)) renderComposer();
}
let renderedScreenshots = null;

export function clearScreenshots(composer, reply) {
  const drafts = screenshotsFor(composer);
  const anchor = screenshotAnchor(reply);
  const draft = drafts?.get(anchor);
  drafts?.delete(anchor);
  for (const image of draft?.images ?? []) URL.revokeObjectURL(image.preview);
}

export async function addScreenshots(context, composer, reply, files) {
  const anchor = screenshotAnchor(reply);
  const isCurrent = () => view === context && context.composer === composer && context.data.canDecide === true &&
    !decisions.composer() && screenshotAnchor(composer.reply) === anchor && !closedQuestion(context, reply) && !sendingMessages.has(composer);
  if (!files.length || !composer || !isCurrent()) return;
  let drafts = screenshotsFor(composer);
  if (!drafts) screenshotDrafts.set(composer, drafts = new Map());
  const draft = drafts.get(anchor) ?? { images: [] };
  if (draft.loading) return;
  drafts.set(anchor, draft);
  draft.loading = true;
  draft.error = '';
  notices.delete(context.id);
  renderComposer();
  try { await stageScreenshots(draft, files, context.data.limits.attachments, isCurrent); }
  finally {
    draft.loading = false;
    if (showingComposer(context.id)) renderComposer();
  }
}

function renderScreenshots(draft, disabled) {
  const list = elements['screenshot-previews'];
  const images = draft?.images ?? null;
  if (renderedScreenshots !== images) {
    renderedScreenshots = images;
    list.replaceChildren(...(images ?? []).map((image, index) => {
      const item = node('li', 'screenshot-preview');
      const preview = node('img');
      preview.alt = image.attachment.name;
      preview.dataset.tooltip = image.attachment.name;
      preview.src = image.preview;
      const remove = button('×', 'icon-button remove-screenshot', () => {
        const hadFocus = document.activeElement === remove;
        URL.revokeObjectURL(image.preview);
        draft.images = draft.images.filter((entry) => entry !== image);
        draft.error = '';
        renderComposer();
        if (hadFocus) {
          const buttons = list.querySelectorAll('button');
          const next = buttons[Math.min(index, buttons.length - 1)] ?? elements['attach-screenshots'];
          (next.disabled ? elements.message : next).focus();
        }
      });
      remove.setAttribute('aria-label', `Remove ${image.attachment.name}`);
      remove.dataset.tooltip = 'Remove image';
      remove.dataset.tooltip = `Remove ${image.attachment.name}`;
      item.append(preview, remove);
      return item;
    }));
  }
  list.hidden = !images?.length;
  for (const button of list.querySelectorAll('button')) button.disabled = disabled;
}
function scheduleNoticeFade(context, notice) {
  const bot = context.id;
  if (!notice || notice.isError || notice.timer || sendingMessages.has(context.composer) || decisions.pendingForBot(bot)) return;
  notice.timer = setTimeout(() => {
    notice.timer = null;
    if (notices.get(bot) !== notice || !showingComposer(bot)) return;
    notice.fading = true;
    renderComposer();
    setTimeout(() => {
      if (notices.get(bot) !== notice) return;
      notices.delete(bot);
      if (showingComposer(bot)) renderComposer();
    }, 400);
  }, 3600);
}
export function fitMessage() {
  const field = elements.message;
  if (!field.offsetParent) return;
  const scroller = elements.conversation;
  const atBottom = timelineAtEnd(scroller);
  field.style.height = 'auto';
  field.style.height = `${field.scrollHeight}px`;
  // Measuring the textarea briefly frees space and can clamp the timeline's scroll position.
  if (atBottom) scroller.scrollTop = scroller.scrollHeight;
}

function renderComposerCopy({ data, events }, target, inboxReply, suggestion) {
  const answering = target?.kind === 'wait';
  const working = ['working', 'thinking'].includes(data.presence);
  const fieldLabel = answering ? 'Write your answer' :
    target ? 'Write a reply' : 'Message';
  elements['decision-reply-context'].hidden = !inboxReply;
  elements['reply-context'].hidden = !target || inboxReply;
  elements['compose-label'].textContent = fieldLabel;
  elements.message.placeholder = suggestion ? `${suggestion.text} (press Enter to use this suggestion)` :
    target ? fieldLabel : working ? `Guide ${data.bot.name} while it works` : `Message ${data.bot.name}`;
  const sendLabel = answering ? 'Send answer' : target ? 'Send reply' : 'Send message';
  elements.send.setAttribute('aria-label', sendLabel);
  elements.send.dataset.tooltip = sendLabel;

  const summaryId = inboxReply ? 'decision-reply-target' : 'reply-summary';
  elements.message.setAttribute('aria-describedby', target ? `${summaryId} notice` : 'notice');
  if (!target) return;

  let summary = target.summary;
  if (!summary && target.messageId) {
    const source = events.get(target.messageId)?.data;
    if (source) summary = answering ? waitText(source) : source.report ?? source.text;
  }
  elements[inboxReply ? 'decision-reply-label' : 'reply-label'].textContent = answering ? 'Answering' : 'Replying to';
  elements[summaryId].textContent = summary || (answering ? waitLabel('user_question') : 'Earlier message');
}

export function renderComposer() {
  const context = view;
  const readOnly = context?.data?.canDecide === false;
  const bot = context?.data?.bot;
  const paused = !!bot?.paused;
  const needsModel = !!bot && !paused && !readOnly && !bot.model;
  const banner = paused ? 'paused' : needsModel ? 'setup' : null;
  const available = !!context?.data && (!!context.composer || readOnly || !!banner);
  const canDecide = available && !!context.composer && !banner;
  const wasComposing = elements.composer.contains(document.activeElement);
  const action = elements['composer-banner-action'];
  const canAct = !!banner && bot.canDelete === true;
  elements['composer-banner'].hidden = !banner;
  elements['composer-banner'].dataset.state = banner ?? '';
  action.hidden = !canAct;
  action.textContent = banner === 'setup' ? 'Choose a model' : 'Resume';
  const title = banner === 'paused' ? `${bot.name} is paused` : banner === 'setup' ? `${bot.name} isn’t set up yet` : '';
  if (elements['composer-banner-title'].textContent !== title) elements['composer-banner-title'].textContent = title;
  const bannerDescription = elements['composer-banner-description'];
  const description = banner === 'paused' ? context.resumeError ||
      (canAct ? 'Resume to send a message.' : 'The Bot’s owner or a Flock administrator can resume it.')
    : banner === 'setup' ? (canAct ? 'Choose a model so this Bot can reply.' : 'The Bot’s owner or a Flock administrator can set it up.')
    : '';
  if (bannerDescription.textContent !== description) bannerDescription.textContent = description;
  bannerDescription.classList.toggle('error', paused && !!context.resumeError);
  elements['composer-container'].hidden = !available;
  elements.composer.hidden = !canDecide;
  elements['view-only-notice'].hidden = !available || !!banner || canDecide;
  if (banner && wasComposing) (canAct ? action : headerFocusTarget()).focus({ preventScroll: true });
  const decision = canDecide ? decisions.composer() : null;
  if (!canDecide) {
    assignDragDepth(0);
    elements.composer.classList.remove('dragover');
    assignComposition(null);
    elements['composer-ghost'].hidden = true;
    elements.message.classList.remove('has-ghost');
    elements.message.value = '';
    elements.message.readOnly = true;
    elements.send.disabled = true;
    return renderScreenshots(null, true);
  }
  const { composer } = context;
  const reply = decision ? null : composer.reply;
  const anchor = decision?.key ?? reply?.messageId;
  if (composition && (composition.composer !== composer || composition.anchor !== anchor)) assignComposition(null);
  const screenshots = decision ? null : screenshotsFor(composer)?.get(screenshotAnchor(reply));
  const notice = notices.get(context.id);
  const body = decision?.text ?? composer.textAt(screenshotAnchor(reply));
  const busy = !!decision?.busy || sendingMessages.has(composer);
  const closedReply = closedQuestion(context, reply);
  elements['cancel-reply'].disabled = busy;
  const suggestion = nextSuggestion(context);
  const target = decision ? { kind: 'wait', summary: decision.item.prompt?.trim() } : reply;
  renderComposerCopy(context, target, !!decision, suggestion);
  const ghost = elements['composer-ghost'];
  ghost.hidden = !suggestion;
  ghost.firstElementChild.textContent = suggestion?.text ?? '';
  elements.message.classList.toggle('has-ghost', !!suggestion);
  if (!composition && elements.message.value !== body) {
    elements.message.value = body;
    fitMessage();
  }
  elements.message.readOnly = busy || closedReply;
  elements['attach-screenshots'].hidden = !!decision;
  elements['attach-screenshots'].disabled = !!decision || busy || closedReply;
  elements['attach-screenshots'].setAttribute('aria-disabled', String(!!screenshots?.loading));
  renderScreenshots(screenshots, busy || !!screenshots?.loading);
  elements.composer.setAttribute('aria-busy', String(busy));
  elements.send.disabled = busy || !!composition || closedReply || !!decision?.blocked || !!screenshots?.loading ||
    !body.trim() || byteLength(body) > composerLimit(context.data.limits, reply, decision);
  elements.composer.classList.toggle('can-send', !elements.send.disabled);
  const screenshotError = !busy && !closedReply && screenshots?.error;
  const needsMessageForImages = !busy && !closedReply && !screenshotError && !notice?.text &&
    !body.trim() && (screenshots?.images?.length ?? 0) > 0 && !screenshots?.loading;
  elements.notice.textContent = notice?.isError ? notice.text :
    (closedReply ? 'This question is closed. You can still copy your reply.' :
      screenshotError || notice?.text || (needsMessageForImages ? 'Add a message to send images.' : ''));
  elements.notice.classList.toggle('error', !!(notice?.isError || screenshotError));
  elements.notice.classList.toggle('fading', !closedReply && !screenshotError && !needsMessageForImages && !!notice?.fading);
  elements.notice.classList.toggle('sr-only', !closedReply && !screenshotError && !needsMessageForImages && !!notice?.quiet);
  scheduleNoticeFade(context, notice);
}
