export const sessionEndLabels = {
  completed: 'Done', cancelled: 'Cancelled', failed: 'Failed',
  interrupted: 'Interrupted', rolled_back: 'Rolled back', ended: 'Ended',
};
export const stateLabels = {
  ...sessionEndLabels,
  queued: 'Queued', deciding: 'Evaluating', sending: 'Sending', running: 'Working', waiting: 'Waiting for you',
  confirmation_pending: 'Confirmation pending', external_waiting: 'Waiting',
  cancelling: 'Cancelling', stopping: 'Confirming stop',
  idle: 'Idle', thinking: 'Thinking', working: 'Working', paused: 'Paused',
};
// Wait copy names the open Bot (`$BOT`) rather than a generic "the Bot". An optional third entry is the
// Bot's own first-person wording for its conversation bubble.
export const waitLabels = {
  user_question: ['Question from $BOT', '$BOT is waiting for your answer.', 'I’m waiting for your answer.'],
  policy_approval: ['Approval needed', '$BOT is waiting for your approval before it continues.', 'I need your approval before I continue.'],
  bot_notes_unavailable: ['$BOT’s notes are unavailable', 'Restore access to the Bot’s Role and memory.'],
  t3_input: ['Action in T3', 'Open the thread in T3 to approve or answer.'],
  auth: ['T3 sign-in needed', 'Sign in again in T3 Code or reconnect this environment in Flock settings.'],
  settings: ['T3 settings need attention', 'Check this T3 environment under Connections in Flock settings.'],
};
// Name what the person will do in the dialog; the ellipsis says it opens a step before anything happens.
export const pendingActionLabels = {
  t3_input: 'Open in T3…',
};
export const pendingKindLabels = {
  user_question: 'Question for you',
  protected_input: 'Secret needed',
  policy_approval: 'Approval needed',
};
// What a person's response to a pending item did.
export const decisionLabels = {
  answer: 'Answered', cancel: 'Stop requested', deny: 'Denied', supply: 'Secret saved',
  allow_once: 'Allowed once', always_allow: 'Allowed for this Flock',
  allow_next_time: 'Allow rule saved for this Flock',
};
export const closedLabels = {
  ...decisionLabels,
  run_finished: 'Work ended',
  session_question: 'The question changed in T3', withdrawn: 'Question withdrawn', request_changed: 'Request changed',
  expired: 'Approval expired',
  stale: 'No longer needed', delivery_confirmed: 'Delivery confirmed', connection_restored: 'Connection restored',
  bot_setup_restored: 'Resumed', action_closed: 'Action closed',
  superseded: 'Replaced', superseded_instruction: 'Replaced by a newer instruction',
};
// Confirms what the response will do, so the person isn't left with a bare "Done".
export const decisionConfirmations = {
  answer: (name) => `Answer recorded for ${name}.`,
  allow_once: () => 'Allowed once.',
  always_allow: () => 'Allowed and saved as a rule for this Flock.',
  allow_next_time: () => 'Allow rule saved for this Flock.',
  deny: () => 'Denied.',
  supply: () => 'Secret saved.',
};
// What to tell the person when the server refuses a message or a response, by error code.
export const responseErrors = {
  invalid_text: 'Check the message and its length.',
  invalid_input: 'Check your response and try again.',
  message_conflict: 'A message was already recorded for this submission. Check the conversation.',
  operation_conflict: 'Flock already processed this action or the pending request changed.',
  decision_conflict: 'Flock could not apply your response. Check it and try again.',
  request_changed: 'The request changed. Review the current request before responding again.',
  already_closed: 'This request was already answered or closed.',
  access_denied: 'You don’t have permission to respond to this request.',
  bot_not_found: 'Bot not found.',
  bot_paused: 'This Bot is paused.',
};
