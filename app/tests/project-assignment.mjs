import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { environmentWithoutFlock, startFlock } from './daemon.mjs';

const [, , executable] = process.argv;
const home = mkdtempSync(join(tmpdir(), 'flock-project-assignment-'));
const identities = join(home, 'identities.json');
const owner = 'Bearer fixture-project-owner';
const editor = 'Bearer fixture-project-editor';
const env = environmentWithoutFlock();
env.FLOCK_WEB_IDENTITIES_FILE = identities;

async function verify(origin) {
  const api = async (path, { as = owner, body, status = 200 } = {}) => {
    const headers = { Authorization: as };
    if (body) Object.assign(headers, { Origin: origin, 'Content-Type': 'application/json', 'X-Flock-Action': '1' });
    const response = await fetch(`${origin}/api/${path}`, {
      method: body ? 'POST' : 'GET', headers, body: JSON.stringify(body), signal: AbortSignal.timeout(5000),
    });
    const text = await response.text();
    assert.equal(response.status, status, `${path}: ${text}`);
    return text && JSON.parse(text);
  };

  const project = { environmentId: 'demo', projectId: 'demo-project' };
  const docs = { ...project, projectId: 'demo-docs' };
  const create = (name, workspace = {}, status = 201) => api('bots', {
    body: { name, avatar: { kind: 'shape', example: 'sprout' }, ...workspace }, status,
  });
  const recent = async (as = owner) => (await api('settings/projects', { as })).recent;
  assert.equal(await recent(), null);
  assert.deepEqual((await api('settings/projects')).environments, [{ id: 'demo', name: 'Demo' }]);
  assert.deepEqual((await api('settings/projects?environmentId=demo')).projects.map(({ id }) => id),
    [project.projectId, docs.projectId]);
  assert.deepEqual((await api('settings/projects?environmentId=unknown')).projects, []);
  await api('settings/projects', { as: 'Bearer invalid', status: 401 });

  const created = await create('Created with project', project);
  assert.equal(created.environmentId, project.environmentId);
  assert.equal(created.projectId, project.projectId);
  assert.equal(created.model || '', '');
  assert.deepEqual(await recent(), project);
  assert.equal(await recent(editor), null);
  await create('Unknown project', { ...project, projectId: 'unknown' }, 400);
  assert.deepEqual(await recent(), project);

  const unassigned = await create('Choose a project later');
  assert.equal(unassigned.projectId || '', '');
  assert.deepEqual(await recent(), project);
  const save = (workspace, status = 200) => api('bots/profile', {
    as: editor, body: { botId: unassigned.id, ...workspace }, status,
  });
  await save({ projectId: project.projectId }, 400);
  assert.equal(await recent(editor), null);

  await api('bots/pause', { body: { botId: unassigned.id, paused: true } });
  assert.equal((await save(project)).projectId, project.projectId);
  assert.deepEqual(await recent(editor), project);
  await save(docs);
  assert.deepEqual(await recent(editor), docs);
  await save(project);
  assert.deepEqual(await recent(editor), project);

  await save({ projectId: '', environmentId: '' });
  assert.deepEqual(await recent(editor), project);
  assert.deepEqual(await recent(), project);
  for (const bot of [created, unassigned]) {
    assert.deepEqual(JSON.parse(execFileSync(executable, ['--home', home, 'tasks', bot.id],
      { encoding: 'utf8', timeout: 5000, env })), []);
  }
}

try {
  writeFileSync(identities, JSON.stringify([
    { person: 'project-owner', authorization: owner },
    { person: 'project-editor', authorization: editor, adminRoot: true },
  ]), { mode: 0o600 });
  const daemon = await startFlock(executable, home, { env });
  try {
    await verify(daemon.origin);
  } finally {
    await daemon.stop();
  }
} finally {
  rmSync(home, { recursive: true, force: true });
}
