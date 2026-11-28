const encoder = new TextEncoder();
const maxImageBytes = 10 * 1024 * 1024;
const maxFileBytes = 50 * 1024 * 1024;
const imageTypes = new Set(['image/png', 'image/jpeg', 'image/gif', 'image/webp']);

function encodeFile(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result.slice(reader.result.indexOf(',') + 1));
    reader.onerror = () => reject(reader.error);
    reader.readAsDataURL(file);
  });
}

export async function stageAttachments(draft, files, maxAttachments, isCurrent) {
  for (const file of files) {
    if (!isCurrent()) return;
    if (draft.images.length >= maxAttachments) {
      draft.error ||= `Attach up to ${maxAttachments} files per message.`;
      return;
    }
    const name = file.name || 'Attachment';
    const mimeType = file.type || 'application/octet-stream';
    const isImage = imageTypes.has(mimeType);
    const limit = isImage ? maxImageBytes : maxFileBytes;
    if (!file.size || file.size > limit) {
      draft.error ||= `${name}: choose a nonempty file up to ${limit / 1024 / 1024} MiB.`;
      continue;
    }
    if (name.length > 255 || /[\u0000-\u001f\u007f]/u.test(name) || name !== name.trim()) {
      draft.error ||= `${name}: the filename cannot be sent to T3.`;
      continue;
    }
    try {
      const dataBase64 = await encodeFile(file);
      if (!isCurrent()) return;
      const attachment = { name, mimeType, dataBase64 };
      draft.images = [...draft.images, { attachment, preview: isImage ? URL.createObjectURL(file) : null }];
    } catch {
      draft.error ||= `${name}: could not read the file.`;
    }
  }
}

export async function screenshotMessage(fields, attachments, previous) {
  const message = { ...fields, attachments };
  const hashes = [];
  for (const { name, mimeType, dataBase64 } of attachments) hashes.push([name, mimeType, await digest(dataBase64)]);
  const fingerprint = await digest(JSON.stringify({ ...fields, attachments: hashes }));
  const clientMessageId = previous?.fingerprint === fingerprint ? previous.clientMessageId : crypto.randomUUID();
  return { payload: { ...message, clientMessageId }, pending: { fingerprint, clientMessageId } };
}

async function digest(text) {
  const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', encoder.encode(text)));
  return btoa(String.fromCharCode(...hash));
}

export function attachmentMessageBody({ attachments, ...message }) {
  const parts = [JSON.stringify(message).slice(0, -1), ',"attachments":['];
  for (const [i, attachment] of attachments.entries()) {
    if (i) parts.push(',');
    parts.push(JSON.stringify(attachment));
  }
  parts.push(']}');
  return new Blob(parts, { type: 'application/json' });
}
