import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { promisify } from 'node:util';

const assets = new URL('../assets/', import.meta.url);
const contentTypes = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };

export const assetUrl = (name) => new URL(name, assets).href;
export const readAsset = (path) => readFile(new URL(`.${path}`, assets), 'utf8');

export async function respondWithAsset(response, pathname, pages = {}) {
  try {
    const page = Object.hasOwn(pages, pathname) ? pages[pathname] : null;
    response.setHeader('Content-Type', page === null ? contentTypes[extname(pathname)] ?? 'application/octet-stream' : 'text/html');
    response.end(page ?? await readFile(new URL(`.${pathname}`, assets)));
  } catch {
    response.writeHead(404).end();
  }
}

export async function withServer(handler, run) {
  const server = createServer(handler);
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  try {
    return await run(`http://127.0.0.1:${server.address().port}`);
  } finally {
    server.close();
  }
}

export const withAssetServer = (pages, run) => withServer((request, response) =>
  respondWithAsset(response, new URL(request.url, 'http://localhost').pathname, pages), run);

async function withScratchDirectory(run) {
  const directory = await mkdtemp(join(tmpdir(), 'flock-dom-'));
  try {
    return await run(directory);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}

export const dumpDom = (chrome, url, { flags = [], budget = 3000 } = {}) => withScratchDirectory(async (profile) => {
  const { stdout } = await promisify(execFile)(chrome, ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', ...flags, `--user-data-dir=${profile}`,
    ...(budget ? [`--virtual-time-budget=${budget}`] : []), '--dump-dom', url], { encoding: 'utf8', timeout: 25000 });
  return stdout;
});

export const dumpFixture = (chrome, html, { flags = [], files = {}, ...options } = {}) => withScratchDirectory(async (directory) => {
  const fixture = join(directory, 'index.html');
  await writeFile(fixture, html);
  for (const [name, content] of Object.entries(files)) await writeFile(join(directory, name), content);
  return dumpDom(chrome, pathToFileURL(fixture).href, { ...options, flags: ['--allow-file-access-from-files', ...flags] });
});

export function requirePass(dom, unfinished, label) {
  const result = dom.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(`${label ? `${label}: ` : ''}${result?.[1] ?? unfinished}`);
}
