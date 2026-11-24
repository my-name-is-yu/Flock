import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

async function exercise(createRecurringRequests) {
  const check = (value, message) => { if (!value) throw new Error(message); };
  const nextTask = () => new Promise((resolve) => setTimeout(resolve));
  const utc = Date.UTC(2026, 0, 15, 14) / 1000;
  const localZone = Intl.DateTimeFormat().resolvedOptions().timeZone;
  const reminderZone = localZone === 'America/New_York' ? 'America/Los_Angeles' : 'America/New_York';
  const reminderTime = reminderZone === 'America/New_York' ? '09:00' : '06:00';
  const reminder = { id: 4, kind: 'reminder', request: 'Reminder', fireAt: utc, timezone: reminderZone };
  const configured = { id: 3, kind: 'schedule', request: 'Configured', cron: '  005 009 * * 7  ',
    timezone: localZone, configId: 'weekly' };
  const advanced = { id: 2, kind: 'schedule', request: 'Monthly', cron: '0 9 1 * *',
    timezone: 'Europe/Paris', nextRunAt: utc };
  let canManage = true;
  let recent = [reminder, configured];
  let reads = 0;
  let holdRead;
  let pendingSignal;
  let added;
  let removed;
  let poll;
  window.setInterval = (refresh) => { poll = refresh; return 1; };
  window.clearInterval = () => { poll = null; };
  const api = async (path, body, { method, signal }) => {
    const url = new URL(path, 'http://localhost/');
    if (method === 'DELETE') {
      removed = url.searchParams.get('id');
      recent = recent.filter((item) => String(item.id) !== removed);
      return {};
    }
    if (body) {
      added = body;
      recent = [{ ...body, id: 5 }, ...recent];
      return {};
    }
    check(url.searchParams.get('active') === '1', 'the active filter was dropped');
    if (url.searchParams.get('before') === '3') return { automations: [advanced], before: 0 };
    reads++;
    const page = { automations: [...recent], before: 3, canManage, unscheduledRequests: ['Unavailable'] };
    if (holdRead) {
      pendingSignal = signal;
      await holdRead;
    }
    return page;
  };

  const settings = createRecurringRequests('release', api);
  document.body.append(settings.count, settings.list, settings.form);
  await nextTask();
  const { list, form, count } = settings;
  const [, configuredWhen, advancedWhen] = list.querySelectorAll('.cron-when');
  const reminderWhen = list.querySelector('.cron-when');
  check(count.data === '3', 'the count omitted an older page');
  check(reminderWhen.textContent.includes(reminderTime) && reminderWhen.textContent.includes(reminderZone),
    'the reminder was formatted in the wrong zone');
  check(configuredWhen.textContent === 'Every Sunday at 09:05', 'the weekly expression was described incorrectly');
  check(advancedWhen.textContent.startsWith('Runs next on ') && !advancedWhen.textContent.includes(advanced.cron),
    'the UI exposed a raw expression for a complex schedule');
  check(list.textContent.includes('Unavailable') && list.querySelectorAll('button').length === 2,
    'configuration warning or read-only configured row was lost');

  const trigger = { kind: 'trigger', source: 'deploy', request: 'Check the deployment' };
  const longCondition = 'release-'.repeat(25) + '<img>';
  recent.push({ ...trigger, id: 6, eventFilter: { branch: 'main' } },
    { ...trigger, id: 7, eventFilter: { branch: 'staging' } },
    { ...trigger, id: 8, eventFilter: { branch: longCondition } });
  poll();
  await nextTask();
  const mainTrigger = list.querySelector('[data-schedule-id="6"]');
  const stagingTrigger = list.querySelector('[data-schedule-id="7"]');
  const removeDescription = (button) => document.getElementById(button.getAttribute('aria-describedby'));
  check(mainTrigger.getAttribute('aria-label').includes('deploy') &&
    removeDescription(mainTrigger)?.textContent === 'Matches branch = "main"' &&
    removeDescription(stagingTrigger)?.textContent === 'Matches branch = "staging"',
    'same-source triggers did not identify their distinct conditions');
  const details = removeDescription(list.querySelector('[data-schedule-id="8"]'))?.closest('details');
  check(details && !details.open, 'a long condition did not start collapsed');
  details.open = true;
  check(details.textContent.includes(longCondition) && !details.querySelector('img'),
    'a long condition was incomplete or interpreted as markup');
  stagingTrigger.click();
  await nextTask();
  check(removed === '7' && !list.querySelector('[data-schedule-id="7"]') &&
    list.querySelector('[data-schedule-id="6"]'), 'removing the staging trigger removed the wrong trigger');

  const draft = form.querySelector('textarea');
  const [repeat] = form.querySelectorAll('select');
  draft.value = 'Draft to keep';
  draft.focus();
  recent = [configured];
  poll();
  await nextTask();
  check(count.data === '2' && !list.textContent.includes('Reminder'), 'an external change stayed stale');
  check(draft.value === 'Draft to keep' && document.activeElement === draft, 'a refresh discarded the draft or focus');

  const oversizedRequest = 'あ'.repeat(5334);
  draft.value = oversizedRequest;
  form.requestSubmit();
  await nextTask();
  check(!added && draft.value === oversizedRequest && !form.querySelector('.cron-error').hidden &&
    form.querySelector('.cron-error').textContent.includes('16,000 bytes'),
    'oversized UTF-8 text was submitted or its draft was lost');
  const validRequest = 'a'.repeat(16000);
  draft.select();
  document.execCommand('insertText', false, validRequest);
  repeat.value = '1-5';
  form.requestSubmit();
  await nextTask();
  check(added?.botId === 'release' && added?.cron === '0 9 * * 1-5' && added.request === validRequest &&
    count.data === '3' && !draft.value, 'adding a request truncated text, left a stale list, or kept the submitted draft');

  let resolveRead;
  holdRead = new Promise((resolve) => { resolveRead = resolve; });
  poll();
  holdRead = null;
  list.querySelector('[data-schedule-id="5"]').click();
  await nextTask();
  check(removed === '5' && pendingSignal.aborted && count.data === '2', 'remove did not supersede a stale read');
  resolveRead();
  await nextTask();
  check(count.data === '2', 'a stale response restored the removed request');

  canManage = false;
  poll();
  await nextTask();
  check(form.hidden && !list.querySelector('button'), 'a Bot member can see controls they cannot use');
  const beforeClose = reads;
  settings.close();
  document.dispatchEvent(new Event('visibilitychange'));
  check(!poll && reads === beforeClose, 'closed settings kept refreshing');
}

const directory = mkdtempSync(join(tmpdir(), 'flock-recurring-requests-'));
try {
  const module = new URL('../assets/recurring-requests.js', import.meta.url);
  const fixture = join(directory, 'index.html');
  writeFileSync(fixture, `<!doctype html><html><body><script type="module">
    import { createRecurringRequests } from ${JSON.stringify(module.href)};
    try {
      await (${exercise})(createRecurringRequests);
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script></body></html>`);
  const output = execFileSync(process.argv[2], ['--headless', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--allow-file-access-from-files', `--user-data-dir=${directory}`,
    '--virtual-time-budget=3000', '--dump-dom', pathToFileURL(fixture).href], { encoding: 'utf8', timeout: 25000 });
  const result = output.match(/<html[^>]*data-flock-result="([^"]*)"/);
  if (result?.[1] !== 'pass') throw new Error(result?.[1] ?? 'The browser did not finish the DOM checks');
} finally {
  rmSync(directory, { recursive: true, force: true });
}
