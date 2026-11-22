import assert from 'node:assert/strict';
import { test } from 'node:test';
import { handleComposerEnter } from '../assets/composer-shortcuts.mjs';

function press(keys = {}, { disabled = false, browser = { navigator: {} } } = {}) {
  const event = Object.assign(new Event('keydown', { cancelable: true }), {
    key: 'Enter', code: 'Enter', ...keys,
  });
  let clicks = 0;
  handleComposerEnter(event, { disabled, click: () => { clicks++; } }, browser);
  return { clicks, prevented: event.defaultPrevented };
}

const sent = { clicks: 1, prevented: true };
const kept = { clicks: 0, prevented: false };
const blocked = { clicks: 0, prevented: true };

test('Enter sends and Shift+Enter leaves the default newline', () => {
  assert.deepEqual(press(), sent);
  assert.deepEqual(press({ code: 'NumpadEnter' }), sent);
  assert.deepEqual(press({ shiftKey: true }), kept);
  assert.deepEqual(press({ key: 'Escape' }), kept);
});

test('Control+Enter and Command+Enter send, other modified Enter keys pass through', () => {
  assert.deepEqual(press({ ctrlKey: true }), sent);
  assert.deepEqual(press({ metaKey: true }), sent);
  assert.deepEqual(press({ altKey: true }), kept);
  assert.deepEqual(press({ ctrlKey: true, altKey: true }), kept);
  assert.deepEqual(press({ ctrlKey: true, shiftKey: true }), kept);
  assert.deepEqual(press({ ctrlKey: true, metaKey: true }), kept);
});

test('Empty or disabled composers and held send shortcuts do not send', () => {
  assert.deepEqual(press({}, { disabled: true }), blocked);
  assert.deepEqual(press({ repeat: true }), blocked);
  assert.deepEqual(press({ repeat: true, ctrlKey: true }), blocked);
  assert.deepEqual(press({ repeat: true, shiftKey: true }), kept);
});

test('IME confirmation and its repeats stay with the input method', () => {
  assert.deepEqual(press({ isComposing: true }), kept);
  assert.deepEqual(press({ isComposing: false, keyCode: 229 }), kept);
  assert.deepEqual(press({ isComposing: true, metaKey: true, repeat: true }), kept);
});

test('Touch keyboard Return stays a newline, with hardware shortcuts where detectable', () => {
  const browser = {
    innerHeight: 800,
    navigator: { maxTouchPoints: 5 },
    visualViewport: { height: 500, scale: 1 },
  };
  assert.deepEqual(press({}, { browser }), kept);
  assert.deepEqual(press({ ctrlKey: true }, { browser }), sent);

  browser.visualViewport.height = 700;
  assert.deepEqual(press({}, { browser }), sent);
  assert.deepEqual(press({ code: '' }, { browser }), kept);
  assert.deepEqual(press({ code: 'Unidentified' }, { browser }), kept);

  browser.visualViewport = { height: 400, scale: 2 };
  assert.deepEqual(press({}, { browser }), sent);

  browser.navigator.virtualKeyboard = { boundingRect: { height: 250 } };
  assert.deepEqual(press({}, { browser }), kept);
});
