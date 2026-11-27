const maxSourceBytes = 20 * 1024 * 1024;
const maxPreparedBytes = 1792 * 1024;
const maxPreparedEdge = 2048;
const encoder = new TextEncoder();
const allowedMimeTypes = new Set(['image/png', 'image/jpeg', 'image/webp']);
const pngSignature = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
const jpegSignature = [0xff, 0xd8, 0xff];
const riffSignature = [0x52, 0x49, 0x46, 0x46];
const webpSignature = [0x57, 0x45, 0x42, 0x50];

class ScreenshotError extends Error {}

function matchesSignature(bytes, signature, offset = 0) {
  return bytes.length >= offset + signature.length && signature.every((byte, index) => bytes[offset + index] === byte);
}

function detectImageMimeType(bytes) {
  if (matchesSignature(bytes, pngSignature)) return 'image/png';
  if (matchesSignature(bytes, jpegSignature)) return 'image/jpeg';
  if (matchesSignature(bytes, riffSignature) && matchesSignature(bytes, webpSignature, 8)) return 'image/webp';
  return null;
}

function attachmentName(file, mimeType) {
  let name = '';
  let bytes = 0;
  const original = file.name.replace(/[\u0000-\u001f\u007f]/g, '').trim();
  const extension = mimeType === 'image/png' ? '.png' : '.jpg';
  const mismatchedExtension = mimeType === 'image/png' ? /\.(?:jpe?g|webp)$/i : /\.(?:png|webp)$/i;
  const filename = original.replace(mismatchedExtension, extension) || `Screenshot${extension}`;
  for (const char of filename) {
    bytes += encoder.encode(char).length;
    if (bytes > 128) break;
    name += char;
  }
  return name;
}

function canvasBlob(canvas, type, quality) {
  return new Promise((resolve) => canvas.toBlob(resolve, type, quality));
}

function hasTransparency(context, { width, height }) {
  const { data } = context.getImageData(0, 0, width, height);
  for (let alpha = 3; alpha < data.length; alpha += 4) {
    if (data[alpha] !== 255) return true;
  }
  return false;
}

async function compressScreenshot(bitmap, longestEdge, mimeType) {
  const canvas = document.createElement('canvas');
  const context = canvas.getContext('2d');
  for (let edge = Math.min(longestEdge, maxPreparedEdge); edge >= 1; edge = Math.floor(edge * 0.75)) {
    const scale = edge / longestEdge;
    canvas.width = Math.max(1, Math.round(bitmap.width * scale));
    canvas.height = Math.max(1, Math.round(bitmap.height * scale));
    context.imageSmoothingQuality = 'high';
    context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
    const png = await canvasBlob(canvas, 'image/png');
    const opaque = mimeType === 'image/jpeg' || !hasTransparency(context, canvas);
    const jpeg = opaque ? await canvasBlob(canvas, 'image/jpeg', 0.85) : null;
    const smallest = jpeg && (!png || jpeg.size < png.size) ? jpeg : png;
    if (smallest?.size <= maxPreparedBytes) return smallest;
  }
  throw new ScreenshotError('Couldn’t prepare this image.');
}

export async function resizeScreenshot(file) {
  if (file.type && !allowedMimeTypes.has(file.type)) throw new ScreenshotError('Choose a PNG, JPEG, or WebP image.');
  if (file.size > maxSourceBytes) throw new ScreenshotError('Choose an image up to 20 MB.');

  const bytes = new Uint8Array(await file.slice(0, 12).arrayBuffer());
  const mimeType = detectImageMimeType(bytes);
  if (!mimeType || (file.type && file.type !== mimeType)) throw new ScreenshotError('Choose a valid PNG, JPEG, or WebP image.');

  const bitmap = await createImageBitmap(file);
  try {
    const edge = Math.max(bitmap.width, bitmap.height);
    if (mimeType !== 'image/webp' && edge <= maxPreparedEdge && file.size <= maxPreparedBytes) {
      return file.type ? file : file.slice(0, file.size, mimeType);
    }
    return await compressScreenshot(bitmap, edge, mimeType);
  } finally {
    bitmap.close();
  }
}

async function screenshotAttachment(file, prepared) {
  const bytes = new Uint8Array(await prepared.arrayBuffer());
  let binary = '';
  for (let index = 0; index < bytes.length; index += 8192)
    binary += String.fromCharCode(...bytes.subarray(index, index + 8192));
  return { name: attachmentName(file, prepared.type), mimeType: prepared.type, dataBase64: btoa(binary) };
}

export async function stageScreenshots(draft, files, maxAttachments, isCurrent) {
  for (const file of files) {
    if (!isCurrent()) return;
    if (draft.images.length >= maxAttachments) {
      draft.error ||= `Attach up to ${maxAttachments} images per message.`;
      return;
    }
    try {
      const prepared = await resizeScreenshot(file);
      if (!isCurrent()) return;
      const attachment = await screenshotAttachment(file, prepared);
      if (!isCurrent()) return;
      const preview = URL.createObjectURL(prepared);
      draft.images = [...draft.images, { attachment, preview }];
    } catch (error) {
      if (isCurrent()) draft.error ||= error instanceof ScreenshotError ? error.message : 'Couldn’t open this PNG, JPEG, or WebP image.';
    }
  }
}

export async function screenshotMessage(fields, attachments, previous) {
  const message = { ...fields, attachments };
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', encoder.encode(JSON.stringify(message))));
  const fingerprint = btoa(String.fromCharCode(...digest));
  const clientMessageId = previous?.fingerprint === fingerprint ? previous.clientMessageId : crypto.randomUUID();
  return { payload: { ...message, clientMessageId }, pending: { fingerprint, clientMessageId } };
}
