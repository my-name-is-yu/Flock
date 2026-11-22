export function hasQuestionChoices(choices) {
  return Array.isArray(choices) && choices.length > 0 &&
    choices.every((choice) => typeof choice === 'string' && choice.trim());
}

export function questionReplyButton(open, dialog = false) {
  const reply = document.createElement('button');
  reply.type = 'button';
  reply.className = dialog ? 'dialog-button quiet' : 'text-button question-reply';
  reply.textContent = 'Write a reply';
  reply.addEventListener('click', open);
  return reply;
}

export function updateQuestionChoiceForm(form, answer, busy) {
  const options = form.querySelector('fieldset');
  options.disabled = busy;
  let hasAnswer = false;
  for (const input of options.querySelectorAll('input')) {
    input.checked = input.value === answer;
    hasAnswer ||= input.checked;
  }
  form.querySelector('button').disabled = busy || !hasAnswer;
}

export function questionChoiceForm({ key, prompt, choices, answer, busy, dialog, select, submit }) {
  const form = document.createElement('form');
  form.className = 'question-choice-form';
  form.dataset.questionKey = key;

  const options = document.createElement('fieldset');
  options.className = 'question-options';
  const legend = document.createElement('legend');
  legend.className = 'sr-only';
  legend.textContent = prompt;
  options.append(legend);
  for (const choice of choices) {
    const label = document.createElement('label');
    label.className = 'question-option';
    const input = document.createElement('input');
    input.type = 'radio';
    input.name = 'answer';
    input.value = choice;
    const text = document.createElement('span');
    text.textContent = choice;
    label.append(input, text);
    options.append(label);
  }

  const confirm = document.createElement('button');
  confirm.type = 'submit';
  confirm.className = dialog ? 'dialog-button primary' : 'button question-confirm';
  confirm.textContent = 'Send answer';
  form.append(options, confirm);
  form.addEventListener('change', (event) => {
    if (!options.disabled && event.target.checked) select(event.target.value);
  });
  form.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && event.target.matches('input[type=radio]')) event.preventDefault();
  });
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    const selected = options.querySelector('input:checked');
    if (!confirm.disabled && selected) submit(selected.value);
  });
  updateQuestionChoiceForm(form, answer, busy);
  return form;
}
