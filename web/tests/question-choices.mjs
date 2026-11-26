import { assetUrl, dumpFixture, requirePass } from './dom-harness.mjs';

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

const html = `<!doctype html><html><body><script type="module">
  import { questionChoiceForm, updateQuestionChoiceForm } from ${JSON.stringify(assetUrl('question-choices.js'))};
  try {
    (${exercise})(questionChoiceForm, updateQuestionChoiceForm);
    document.documentElement.dataset.flockResult = 'pass';
  } catch (error) {
    document.documentElement.dataset.flockResult = error.message;
  }
</script></body></html>`;
requirePass(await dumpFixture(process.argv[2], html), 'The browser did not finish the DOM checks');
