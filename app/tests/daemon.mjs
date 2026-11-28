import { spawn } from 'node:child_process';
import { once } from 'node:events';

export const environmentWithoutFlock = () =>
  Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith('FLOCK_')));

// Starts `flock run --fixed` and resolves with its Web UI address once it has printed it.
export async function startFlock(executable, home, { env, flags = ['--port', '0'] } = {}) {
  const child = spawn(executable, ['--home', home, 'run', '--fixed', ...flags], { env, stdio: ['ignore', 'pipe', 'pipe'] });
  const closed = once(child, 'close');
  let stdout = '';
  let stderr = '';
  child.stderr.setEncoding('utf8').on('data', (text) => { stderr += text; });
  const ready = new Promise((resolve, reject) => {
    child.stdout.setEncoding('utf8').on('data', (text) => {
      stdout += text;
      const match = stdout.match(/Flock Web UI at (http:\/\/127\.0\.0\.1:\d+)/);
      if (match) resolve(match[1]);
    });
    child.once('error', reject);
    child.once('close', (code) => reject(new Error(`Flock exited during startup (${code}): ${stderr}`)));
  });
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`Flock did not start: ${stderr}`)), 5000);
  });
  const stop = async () => {
    child.kill();
    await closed;
  };
  try {
    return { origin: await Promise.race([ready, timeout]), stop };
  } catch (error) {
    await stop();
    throw error;
  } finally {
    clearTimeout(timer);
  }
}
