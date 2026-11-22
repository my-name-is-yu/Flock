const maxFiles = 4;
const maxFileBytes = 64 * 1024;
const maxAttachmentBytes = 98_000;
const maxMessageBytes = 128 * 1024;
// Match the attachment inspector's limits in src/attachments.cpp.
const maxImageEdge = 8192;
const maxImagePixels = 4_194_304;
const encoder = new TextEncoder();
const jsonBytes = (value) => encoder.encode(JSON.stringify(value));
const signatures = {
  'image/png': [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a],
  'image/jpeg': [0xff, 0xd8, 0xff],
};

export class ScreenshotError extends Error {}

function attachmentName(file, mimeType) {
  let name = '';
  let bytes = 0;
  for (const char of file.name.replace(/[\u0000-\u001f\u007f]/g, '').trim()) {
    bytes += encoder.encode(char).length;
    if (bytes > 128) break;
    name += char;
  }
  return name || (mimeType === 'image/png' ? 'Screenshot.png' : 'Screenshot.jpg');
}

export async function encodeScreenshot(file, attachments) {
  if (attachments.length >= maxFiles) throw new ScreenshotError('Attach up to 4 screenshots per message.');
  if (file.type && !Object.hasOwn(signatures, file.type)) throw new ScreenshotError('Choose a PNG or JPEG screenshot.');
  if (file.size > maxFileBytes) throw new ScreenshotError('Each screenshot must be 64 KiB or smaller.');

  let bytes;
  try { bytes = new Uint8Array(await file.arrayBuffer()); }
  catch { throw new ScreenshotError('Could not read the screenshot.'); }
  const mimeType = Object.keys(signatures).find((type) => signatures[type].every((byte, index) => bytes[index] === byte));
  if (!mimeType || (file.type && file.type !== mimeType)) throw new ScreenshotError('Choose a valid PNG or JPEG screenshot.');

  let binary = '';
  for (let index = 0; index < bytes.length; index += 8192)
    binary += String.fromCharCode(...bytes.subarray(index, index + 8192));
  const attachment = { name: attachmentName(file, mimeType), mimeType, dataBase64: btoa(binary) };
  if (jsonBytes([...attachments, attachment]).length > maxAttachmentBytes)
    throw new ScreenshotError('Screenshots together exceed the 98,000-byte encoded limit. Remove one or choose smaller files.');
  return attachment;
}

export async function stageScreenshots(draft, files, isCurrent) {
  for (const file of files) {
    if (!isCurrent()) return;
    try {
      const attachment = await encodeScreenshot(file, draft.images.map((image) => image.attachment));
      if (!isCurrent()) return;
      const image = await createImageBitmap(file);
      const { width, height } = image;
      image.close();
      if (!isCurrent()) return;
      if (width > maxImageEdge || height > maxImageEdge || width * height > maxImagePixels)
        throw new ScreenshotError(`Crop or resize ${attachment.name} to at most 8,192 pixels per side and 4,194,304 pixels total.`);
      const preview = URL.createObjectURL(file);
      draft.images = [...draft.images, { attachment, preview }];
    } catch (error) {
      if (isCurrent()) draft.error ||= error instanceof ScreenshotError ? error.message : 'Could not open this PNG or JPEG screenshot.';
    }
  }
}

export async function screenshotMessage(fields, attachments, previous) {
  const message = { ...fields, attachments };
  const bytes = jsonBytes({ ...message, clientMessageId: previous?.clientMessageId ?? '0'.repeat(36) });
  if (bytes.length > maxMessageBytes)
    throw new ScreenshotError('This message and its screenshots are too large together. Shorten the message or remove a screenshot.');

  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', jsonBytes(message)));
  const fingerprint = btoa(String.fromCharCode(...digest));
  const clientMessageId = previous?.fingerprint === fingerprint ? previous.clientMessageId : crypto.randomUUID();
  return { payload: { ...message, clientMessageId }, pending: { fingerprint, clientMessageId } };
}
