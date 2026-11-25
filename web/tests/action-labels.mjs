import assert from 'node:assert/strict';
import { actionLabel, approvalSummary } from '../assets/action-labels.mjs';

assert.equal(actionLabel('environment.launch'), 'Start T3 Sessions');
assert.equal(actionLabel('memory.write'), 'Edit Bot memory');
assert.equal(actionLabel('custom.action'), 'custom.action');
const launch = { action: 'environment.launch', target: 'env-demo' };
const preview = JSON.stringify({ environmentId: 'env-demo', tool: 'delegate',
  instruction: 'Draft the release notes.\n\n[Flock action ID: flock-c709cd1e]' });
assert.equal(approvalSummary(preview, { action: launch.action }), 'Draft the release notes.\nEnvironment: env-demo');
assert.equal(approvalSummary(preview, launch), 'Draft the release notes.');
const delegate = JSON.stringify({ environmentId: 'env-demo', projectId: 'proj-demo', questionId: '', tool: 'delegate',
  run: { link: '', runId: '', threadId: '' },
  instruction: 'Draft the 2.4 release notes\n\nTask intent:\nDraft the 2.4 release notes\n\nCompletion criteria:\nThe notes list every merged PR.\n\n[Flock action ID: flock-c709cd1e]' });
assert.equal(approvalSummary(delegate, launch),
  'Draft the 2.4 release notes\nDone when: The notes list every merged PR.\nProject: proj-demo');
assert.equal(approvalSummary(JSON.stringify({ tool: 'delegate',
  instruction: 'Fix the test\n\nTask intent:\nKeep CI green\n\nCompletion criteria:\n' }), launch),
  'Fix the test\nGoal: Keep CI green');
const ambiguousInstruction = 'Report the following text\n\nTask intent:\nLiteral text\n\nTask intent:\nReport\n\nCompletion criteria:\nDone';
assert.equal(approvalSummary(JSON.stringify({ tool: 'delegate', instruction: ambiguousInstruction }), launch), ambiguousInstruction);
const existing = JSON.stringify({ environmentId: 'env-demo', projectId: 'project', tool: 'start_in_session',
  run: { threadId: 'session-42', runId: '' }, instruction: 'Check staging' });
assert.match(approvalSummary(existing, launch), /Tool: start_in_session.*session-42/s);
const answer = JSON.stringify({ tool: 'answer_session_question', questionId: 'question-12', run: { threadId: 'session-42' } });
assert.match(approvalSummary(answer, { action: 't3.send', target: 'env-demo/session-42' }), /Tool: answer_session_question.*Question Id: question-12.*session-42/s);
assert.equal(approvalSummary(JSON.stringify({ prNumber: 0, draft: false, labels: [] })), 'Pull request: 0\nDraft: false\nLabels: []');
assert.equal(approvalSummary(JSON.stringify({ text: 'release' }), { action: 'channel.post', target: 'release' }), 'Text: release');
assert.equal(approvalSummary(JSON.stringify({ body: 'Ship it' })), 'Body: Ship it');
assert.equal(approvalSummary(JSON.stringify({ environmentId: 'env-demo' })), 'Environment: env-demo');
assert.equal(approvalSummary(JSON.stringify(['revert_memory_edit', { docId: 'notes', revisionId: 12, baseVersion: 20 }]),
  { action: 'shared_memory.write', target: 'notes' }),
  'revert memory edit\nDoc Id: notes\nRevision Id: 12\nBase Version: 20');
assert.equal(approvalSummary(JSON.stringify('Prefer short release notes.'), { action: 'memory.write' }),
  'Prefer short release notes.');
assert.equal(approvalSummary(JSON.stringify({ body: '' })), 'Body: ');
assert.equal(approvalSummary('plain preview'), 'plain preview');
assert.equal(approvalSummary(''), '');
assert.equal(approvalSummary(JSON.stringify(['git_update_pr', { prNumber: 42, state: 'closed' }])),
  'git update pr\nPull request: 42\nState: closed');
assert.equal(approvalSummary(JSON.stringify(['send', { text: 'Release notes', destination: 'release', flags: [true] }])),
  'send\nText: Release notes\nDestination: release\nFlags: [true]');
