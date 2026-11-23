import assert from 'node:assert/strict';
import { actionLabel, approvalSummary } from '../assets/action-labels.mjs';

assert.equal(actionLabel('environment.launch'), 'Start T3 Sessions');
assert.equal(actionLabel('custom.action'), 'custom.action');
const preview = JSON.stringify({ environmentId: 'env-demo', tool: 'delegate',
  instruction: 'Draft the release notes.\n\n[Flock action ID: flock-c709cd1e]' });
assert.equal(approvalSummary(preview), 'Environment: env-demo\nTool: delegate\nInstruction: Draft the release notes.');
assert.equal(approvalSummary(JSON.stringify({ body: 'Ship it' })), 'Body: Ship it');
assert.equal(approvalSummary(JSON.stringify({ environmentId: 'env-demo' })), 'Environment: env-demo');
assert.equal(approvalSummary('plain preview'), 'plain preview');
assert.equal(approvalSummary(''), '');
assert.equal(approvalSummary(JSON.stringify(['git_update_pr', { prNumber: 42, state: 'closed' }])),
  'git update pr\nPull request: 42\nState: closed');
assert.equal(approvalSummary(JSON.stringify(['send', { text: 'Release notes', destination: 'release', flags: [true] }])),
  'send\nText: Release notes\nDestination: release\nFlags: [true]');
