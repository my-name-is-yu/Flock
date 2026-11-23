import assert from 'node:assert/strict';
import { actionLabel, approvalSummary } from '../assets/action-labels.mjs';

assert.equal(actionLabel('environment.launch'), 'Start T3 Sessions');
assert.equal(actionLabel('custom.action'), 'custom.action');
const preview = JSON.stringify({ environmentId: 'env-demo', tool: 'delegate',
  instruction: 'Draft the release notes.\n\n[Flock action ID: flock-c709cd1e]' });
assert.equal(approvalSummary(preview), 'Draft the release notes.\nEnvironment: env-demo');
assert.equal(approvalSummary(preview, 'env-demo'), 'Draft the release notes.');
const delegate = JSON.stringify({ environmentId: 'env-demo', projectId: 'proj-demo', questionId: '', tool: 'delegate',
  run: { link: '', runId: '', threadId: '' },
  instruction: 'Draft the 2.4 release notes\n\nTask intent:\nDraft the 2.4 release notes\n\nCompletion criteria:\nThe notes list every merged PR.\n\n[Flock action ID: flock-c709cd1e]' });
assert.equal(approvalSummary(delegate, 'env-demo'),
  'Draft the 2.4 release notes\nDone when: The notes list every merged PR.\nProject: proj-demo');
assert.equal(approvalSummary(JSON.stringify({ instruction: 'Fix the test\n\nTask intent:\nKeep CI green\n\nCompletion criteria:\n' })),
  'Fix the test\nGoal: Keep CI green');
assert.equal(approvalSummary(JSON.stringify({ prNumber: 0, draft: false, labels: [] })), 'Pull request: 0\nDraft: false');
assert.equal(approvalSummary(JSON.stringify({ body: 'Ship it' })), 'Body: Ship it');
assert.equal(approvalSummary(JSON.stringify({ environmentId: 'env-demo' })), 'Environment: env-demo');
assert.equal(approvalSummary('plain preview'), 'plain preview');
assert.equal(approvalSummary(''), '');
assert.equal(approvalSummary(JSON.stringify(['git_update_pr', { prNumber: 42, state: 'closed' }])),
  'git update pr\nPull request: 42\nState: closed');
assert.equal(approvalSummary(JSON.stringify(['send', { text: 'Release notes', destination: 'release', flags: [true] }])),
  'send\nText: Release notes\nDestination: release\nFlags: [true]');
