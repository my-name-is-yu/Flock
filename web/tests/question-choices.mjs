import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

function exercise(questionChoiceForm, updateQuestionChoiceForm) {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const submitted = [];
  let answer;
  const options = {
    key: 'task:question:revision:generation',
    prompt: 'Which branch?',
    choices: ['main', 'release'],
    busy: false,
    select(value) {
      answer = value;
      updateQuestionChoiceForm(inline, value, false);
    },
    submit: (value) => submitted.push(value),
  };
  const inline = questionChoiceForm(options);
  document.body.append(inline);
  const [main, release] = inline.querySelectorAll('input');
  const send = inline.querySelector('button');
  check(main.labels[0].textContent === 'main' && inline.querySelector('legend').textContent === options.prompt,
    'The choices lost their labels or question');
  check(!main.checked && !release.checked && send.disabled, 'A choice was preselected');
  main.click();
  release.click();
  check(release.checked && !main.checked && !send.disabled && !submitted.length,
    'Selecting an option submitted or left the wrong answer selected');

  check(!release.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })),
    'Enter on a radio did not suppress implicit submission');
  check(!submitted.length, 'Enter on a radio submitted the answer');

  const dialog = questionChoiceForm({ ...options, dialog: true, answer, busy: true });
  document.body.append(dialog);
  const dialogSend = dialog.querySelector('button');
  check(dialog.querySelector('input:checked')?.value === 'release' &&
    dialog.querySelector('fieldset').disabled && dialogSend.disabled, 'Busy render lost or unlocked the selection');
  dialogSend.click();
  check(!submitted.length, 'A disabled confirmation sent an answer');
  updateQuestionChoiceForm(dialog, answer, false);
  dialogSend.click();
  check(submitted.length === 1 && submitted[0] === 'release', 'Confirmation did not send the selected answer');
  updateQuestionChoiceForm(dialog, 'an option that was removed', false);
  check(dialogSend.disabled && !dialog.querySelector('input:checked'), 'An unavailable option remained sendable');
}

const directory = mkdtempSync(join(tmpdir(), 'flock-question-choices-'));
try {
  const module = new URL('../assets/question-choices.js', import.meta.url);
  const html = `<!doctype html><html><body><script type="module">
    import { questionChoiceForm, updateQuestionChoiceForm } from ${JSON.stringify(module.href)};
    try {
      (${exercise})(questionChoiceForm, updateQuestionChoiceForm);
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script></body></html>`;
  const fixture = join(directory, 'index.html');
  writeFileSync(fixture, html);
  const output = execFileSync(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--allow-file-access-from-files', `--user-data-dir=${directory}`,
    '--virtual-time-budget=3000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
  const result = output.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the DOM checks');
} finally {
  rmSync(directory, { recursive: true, force: true });
}
