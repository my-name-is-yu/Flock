import { enableBackdropDismiss } from './dialog-backdrop.mjs';
import { node } from './dom.js';

const glyphs = {
  file: '<path d="M7 3h7l4 4v14H7z M14 3v5h4 M10 12h5 M10 16h5"/>',
  download: '<path d="M12 3v12m-4-4 4 4 4-4M5 16v4h14v-4"/>',
};

function glyph(name) {
  const span = node('span', 'glyph');
  span.setAttribute('aria-hidden', 'true');
  span.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round">${glyphs[name]}</svg>`;
  return span;
}

function fileName(name) {
  const label = node('span', 'chat-file-name');
  const dot = name.lastIndexOf('.');
  const hasExtension = dot > 0 && name.length - dot <= 12;
  label.title = name;
  label.append(node('bdi', 'chat-file-stem', hasExtension ? name.slice(0, dot) : name));
  if (hasExtension) label.append(node('span', null, name.slice(dot)));
  return label;
}

function fileSize(bytes) {
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
  if (bytes >= 1024) return `${Math.round(bytes / 1024)} KiB`;
  return `${bytes} B`;
}

function setDownload(link, file) {
  link.href = `${file.url}&download=1`;
  link.download = file.name;
  link.setAttribute('aria-label', `Download ${file.name}`);
}

function fileCard(file, note = fileSize(file.sizeBytes), available = true) {
  const card = node(available ? 'a' : 'div', 'chat-file-card');
  const label = node('span', 'chat-file-label');
  label.append(fileName(file.name), node('span', 'chat-file-note', note));
  card.append(glyph('file'), label);
  if (available) {
    card.append(glyph('download'));
    card.dataset.timelineAction = `download-attachment-${file.id}`;
    setDownload(card, file);
  }
  return card;
}

async function originalAvailable(file) {
  try {
    const response = await fetch(file.url, { method: 'HEAD' });
    return response.ok;
  } catch {
    return false;
  }
}

async function replaceFailedPreview(element, file) {
  const available = await originalAvailable(file);
  if (!element.isConnected) return;
  const card = fileCard(file, available ? 'Preview unavailable' : 'Attachment unavailable', available);
  if (element.id) card.id = element.id;
  if (!available) card.tabIndex = -1;
  const restoreFocus = element.contains(document.activeElement);
  element.replaceWith(card);
  if (restoreFocus) card.focus({ preventScroll: true });
}

let viewer;
let playingMedia;

function photoViewer() {
  const dialog = document.getElementById('attachment-preview');
  const title = document.getElementById('attachment-preview-title');
  const stage = document.getElementById('attachment-preview-stage');
  const download = document.getElementById('attachment-preview-download');
  const previous = document.getElementById('attachment-preview-previous');
  const next = document.getElementById('attachment-preview-next');
  const close = document.getElementById('attachment-preview-close');
  let photos = [], index = 0, opener;

  const show = (position) => {
    index = (position + photos.length) % photos.length;
    const file = photos[index];
    title.textContent = file.name;
    download.hidden = false;
    const image = node('img', 'attachment-preview-image');
    image.alt = file.name;
    image.addEventListener('error', async () => {
      const available = await originalAvailable(file);
      if (!image.isConnected) return;
      image.replaceWith(node('p', 'attachment-preview-error', available ? 'Preview unavailable.' : 'Attachment unavailable.'));
      download.hidden = !available;
    }, { once: true });
    image.src = file.url;
    stage.replaceChildren(image);
    setDownload(download, file);
    previous.hidden = next.hidden = photos.length < 2;
  };

  previous.addEventListener('click', () => show(index - 1));
  next.addEventListener('click', () => show(index + 1));
  close.addEventListener('click', () => dialog.close());
  dialog.addEventListener('keydown', (event) => {
    if (photos.length < 2 || !['ArrowLeft', 'ArrowRight'].includes(event.key)) return;
    event.preventDefault();
    show(index + (event.key === 'ArrowLeft' ? -1 : 1));
  });
  dialog.addEventListener('close', () => {
    stage.replaceChildren();
    photos = [];
    if (!opener?.isConnected && opener?.id) document.getElementById(opener.id)?.focus({ preventScroll: true });
    else opener?.focus({ preventScroll: true });
    opener = null;
  });
  enableBackdropDismiss(dialog);
  return {
    open(files, file, control) {
      photos = files;
      opener = control;
      show(photos.indexOf(file));
      dialog.showModal();
    },
    close() {
      if (dialog.open) dialog.close();
    },
  };
}

export function closeConversationAttachments() {
  viewer?.close();
  playingMedia?.pause();
}

function photoTile(file, photos, messageId) {
  const tile = node('button', 'chat-photo');
  const image = node('img');
  tile.type = 'button';
  tile.id = `attachment-${messageId}-${file.id}`;
  tile.dataset.timelineAction = `preview-attachment-${file.id}`;
  tile.setAttribute('aria-label', `Open ${file.name}`);
  tile.setAttribute('aria-haspopup', 'dialog');
  tile.title = file.name;
  image.alt = '';
  image.loading = 'lazy';
  image.decoding = 'async';
  image.addEventListener('error', () => void replaceFailedPreview(tile, file), { once: true });
  image.src = file.url;
  tile.append(image);
  tile.addEventListener('click', () => {
    viewer ??= photoViewer();
    viewer.open(photos, file, tile);
  });
  return tile;
}

function mediaCard(file) {
  const card = node('div', `chat-media chat-media-${file.preview}`);
  const media = node(file.preview);
  media.controls = true;
  media.preload = 'metadata';
  if (file.preview === 'video') media.playsInline = true;
  media.setAttribute('aria-label', file.name);
  const download = node('a', 'chat-media-download');
  download.dataset.timelineAction = `download-attachment-${file.id}`;
  setDownload(download, file);
  download.append(glyph('download'));
  const caption = node('div', 'chat-media-caption');
  caption.append(fileName(file.name), download);
  media.addEventListener('error', () => void replaceFailedPreview(card, file), { once: true });
  media.addEventListener('play', () => {
    if (playingMedia !== media) playingMedia?.pause();
    playingMedia = media;
  });
  media.src = file.url;
  card.append(media, caption);
  return card;
}

export function messageAttachments(botId, messageId, files) {
  const list = node('div', 'chat-attachments');
  const items = files.map((file) => ({
    ...file, url: '/api/conversation/attachment?' + new URLSearchParams({ bot: botId, messageId, attachmentId: file.id }),
  }));
  const photos = items.filter((file) => file.preview === 'image');
  let group;
  for (const file of items) {
    if (file.preview === 'image') {
      if (!group) {
        group = node('div', 'chat-photos');
        list.append(group);
      }
      group.append(photoTile(file, photos, messageId));
    } else {
      group = null;
      list.append(file.preview === 'audio' || file.preview === 'video' ? mediaCard(file) : fileCard(file));
    }
  }
  return list;
}
