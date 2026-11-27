import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const executable = process.argv[2];
const home = mkdtempSync(join(tmpdir(),'flock-system-settings-'));
const systemSettingsFile = join(home,'system-settings.json');
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith('FLOCK_')));
const occupied = createServer();
let daemon;

async function start(...flags) {
  const child = spawn(executable,['--home',home,'run','--fixed',...flags],{
    env, stdio: ['ignore','pipe','pipe'],
  });
  let stdout = '';
  let stderr = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stderr.on('data',text => { stderr += text; });
  const closed = new Promise(resolve => child.once('close',resolve));
  const ready = new Promise((resolve,reject) => {
    child.stdout.on('data',text => {
      stdout += text;
      const match = stdout.match(/Flock Web UI at (http:\/\/127\.0\.0\.1:\d+)/);
      if (match) resolve(match[1]);
    });
    child.once('error',reject);
    child.once('close',code => reject(new Error(`Flock exited before startup (${code}): ${stderr}`)));
  });
  let timer;
  const timeout = new Promise((_,reject) => {
    timer = setTimeout(() => reject(new Error(`Flock did not start: ${stderr}`)),5000);
  });
  try {
    const origin = await Promise.race([ready,timeout]);
    return { child, closed, origin };
  } catch (error) {
    child.kill();
    await closed;
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

async function stop() {
  if (!daemon) return;
  daemon.child.kill();
  await daemon.closed;
  daemon = undefined;
}

async function read() {
  const response = await fetch(`${daemon.origin}/api/settings/installation`,{ signal: AbortSignal.timeout(5000) });
  assert.equal(response.status,200);
  return (await response.json()).system;
}

async function change(patch, status = 200) {
  const response = await fetch(`${daemon.origin}/api/settings/installation/system`,{
    method: 'POST',
    headers: { Origin: daemon.origin, 'Content-Type': 'application/json', 'X-Flock-Action': '1' },
    body: JSON.stringify(patch),
    signal: AbortSignal.timeout(5000),
  });
  assert.equal(response.status,status,await response.text());
}

try {
  occupied.listen(0,'127.0.0.1');
  await once(occupied,'listening');
  const nextPort = occupied.address().port;
  daemon = await start();
  const initial = await read();
  assert.deepEqual(initial.saved,{ port: null, sessionRead: null, remoteAddress: null });
  assert.deepEqual(initial.next,{ port: null, sessionRead: 'on_demand', remoteAddress: null });
  assert.equal(initial.active.sessionRead,'on_demand');
  assert.deepEqual(initial.pending,{ port: false, sessionRead: false, remoteAddress: false },'An automatic port must not look pending');

  await change({ port: nextPort, sessionRead: 'invalid' },409);
  assert.deepEqual(await read(),initial,'Invalid settings must not be applied in part');
  await change({ sessionRead: 'prefill' });
  assert.deepEqual((await read()).pending,{ port: false, sessionRead: true, remoteAddress: false },'Only the changed setting may be pending');
  await change({ port: nextPort });
  assert.deepEqual(await read(),{
    active: initial.active,
    saved: { port: nextPort, sessionRead: 'prefill', remoteAddress: null },
    next: { port: nextPort, sessionRead: 'prefill', remoteAddress: null },
    overrides: { port: false, sessionRead: false, remoteAddress: false },
    pending: { port: true, sessionRead: true, remoteAddress: false },
  });
  assert.equal(statSync(systemSettingsFile).mode & 0o777,0o600);

  await stop();
  daemon = await start();
  assert.deepEqual((await read()).active,{ port: initial.active.port, sessionRead: 'prefill', remoteAddress: null });
  assert.deepEqual((await read()).pending,{ port: true, sessionRead: false, remoteAddress: false },'Busy port must stay pending while the earlier port serves requests');

  await stop();
  const released = once(occupied,'close');
  occupied.close();
  await released;
  daemon = await start();
  const applied = await read();
  assert.deepEqual(applied.active,applied.saved);
  assert.deepEqual(applied.pending,{ port: false, sessionRead: false, remoteAddress: false });
  await change({ port: null });
  assert.deepEqual(await read(),{ ...applied, saved: { ...applied.saved, port: null }, next: { ...applied.next, port: null },
    pending: { port: true, sessionRead: false, remoteAddress: false } },'Returning to an automatic port must stay pending until restart');
  await change({ port: applied.saved.port });
  assert.deepEqual(await read(),applied);
  for (const address of ['127.0.0.1','127.1','0x7f.1','localhost','::1','0:0:0:0:0:0:0:1','0.0.0.0','0','::','0:0:0:0:0:0:0:0','::ffff:127.0.0.1','mini tailnet','']) await change({ remoteAddress: address },409);
  for (const address of ['100.101.102.103','fd7a:115c:a1e0::1']) await change({ remoteAddress: address });
  await change({ remoteAddress: 'mini.example.ts.net' });
  const remote = await read();
  assert.equal(remote.next.remoteAddress,'mini.example.ts.net');
  assert.equal(remote.active.remoteAddress,null);
  assert.equal(remote.pending.remoteAddress,true,'A new address for other machines must wait for restart');
  await change({ remoteAddress: null });
  assert.deepEqual(await read(),applied);

  await stop();
  const savedSettings = JSON.parse(readFileSync(systemSettingsFile,'utf8'));
  writeFileSync(systemSettingsFile,JSON.stringify({ ...savedSettings, remoteAddress: null }));
  daemon = await start('--port','0','--session-read','on_demand');
  const overridden = await read();
  assert.deepEqual(overridden.overrides,{ port: true, sessionRead: true, remoteAddress: false });
  assert.equal(overridden.active.sessionRead,'on_demand');
  assert.deepEqual(overridden.saved,applied.saved);
  assert.deepEqual(overridden.next,{ port: null, sessionRead: 'on_demand', remoteAddress: null });
  assert.deepEqual(overridden.pending,{ port: false, sessionRead: false, remoteAddress: false },'Launch settings must not look pending');
  await change({ port: null },409);
  await change({ sessionRead: 'off' },409);
  assert.deepEqual(await read(),overridden);
} finally {
  await stop();
  if (occupied.listening) occupied.close();
  rmSync(home,{ recursive: true, force: true });
}
