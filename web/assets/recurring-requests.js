const weekdays = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const dateOptions = { dateStyle: 'medium', timeStyle: 'short', hourCycle: 'h23' };
const localTime = new Intl.DateTimeFormat('en', dateOptions);
const browserZone = localTime.resolvedOptions().timeZone;
const maxRequestBytes = 16000;

function node(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text != null) element.textContent = text;
  return element;
}

function zoneName(zone) {
  try {
    return new Intl.DateTimeFormat('en', { timeZone: zone, timeZoneName: 'longGeneric' })
      .formatToParts(new Date()).find((part) => part.type === 'timeZoneName')?.value ?? zone;
  } catch (error) {
    if (error instanceof RangeError) return zone;
    throw error;
  }
}

function repeatDescription(cron) {
  const match = /^(\d+)\s+(\d+)\s+\*\s+\*\s+(\*|1-5|\d+)$/.exec(cron.trim());
  if (!match) return null;
  const [, minute, hour, day] = match;
  if (Number(minute) > 59 || Number(hour) > 23) return null;
  const time = [hour, minute].map((field) => String(Number(field)).padStart(2, '0')).join(':');
  if (day === '*') return `Every day at ${time}`;
  if (day === '1-5') return `Weekdays at ${time}`;
  if (Number(day) <= 7) return `Every ${weekdays[Number(day) % 7]} at ${time}`;
  return null;
}

function dateTime(time, zone) {
  let formatter = localTime;
  if (zone !== browserZone) {
    try { formatter = new Intl.DateTimeFormat('en', { ...dateOptions, timeZone: zone }); }
    catch (error) { if (!(error instanceof RangeError)) throw error; }
  }
  return { text: formatter.format(time * 1000), zone: formatter.resolvedOptions().timeZone };
}

function scheduleWhen(item) {
  if (item.kind === 'trigger') return { text: `When an event arrives from ${item.source}` };
  if (item.kind === 'reminder') {
    const { text, zone } = dateTime(item.fireAt, item.timezone);
    return { text: `Once, ${text}`, zone };
  }
  const repeat = repeatDescription(item.cron);
  if (repeat) return { text: repeat, zone: item.timezone };
  const { text, zone } = dateTime(item.nextRunAt, item.timezone);
  return { text: `Runs next on ${text}`, zone };
}

function scheduleRow(item, canManage, removeSchedule) {
  const row = node('div', 'cron-row');
  const when = node('p', 'cron-when');
  const timing = scheduleWhen(item);
  when.append(node('span', null, timing.text));
  if (timing.zone && timing.zone !== browserZone) when.append(node('span', 'cron-zone', zoneName(timing.zone)));
  const text = node('div', 'cron-text');
  text.append(when);
  const filter = item.kind === 'trigger' && Object.entries(item.eventFilter ?? {})
    .map(([key, value]) => `${key} = ${JSON.stringify(value)}`).join(', ');
  let conditions;
  if (filter) {
    conditions = node('p', 'cron-filter', `Matches ${filter}`);
    conditions.id = `cron-conditions-${item.id}`;
    if (filter.length > 160) {
      const details = node('details', 'cron-filter');
      details.append(node('summary', null, 'Event conditions'), conditions);
      text.append(details);
    } else text.append(conditions);
  }
  text.append(node('p', 'cron-request', item.request));
  row.append(text);

  if (item.configId) {
    text.append(node('p', 'cron-note', 'Set in this Bot’s configuration'));
  } else if (canManage) {
    const remove = node('button', 'text-button cron-remove', 'Remove');
    const error = node('p', 'cron-error');
    error.setAttribute('role', 'status');
    error.hidden = true;
    text.append(error);
    remove.type = 'button';
    remove.dataset.scheduleId = item.id;
    remove.setAttribute('aria-label', item.kind === 'trigger'
      ? `Remove “${item.request}” triggered by ${item.source}`
      : `Remove “${item.request}”`);
    if (conditions) remove.setAttribute('aria-describedby', conditions.id);
    remove.addEventListener('click', async () => {
      remove.disabled = true;
      error.hidden = true;
      try {
        await removeSchedule(item.id);
      } catch {
        error.textContent = 'Couldn’t remove this request.';
        error.hidden = false;
      } finally {
        remove.disabled = false;
      }
    });
    row.append(remove);
  }
  return row;
}

function scheduleForm(addSchedule) {
  const form = node('form', 'settings-card cron-form');
  form.hidden = true;
  form.append(node('h3', 'settings-card-label', 'Add a recurring request'));

  const request = node('textarea', 'cron-request-input');
  Object.assign(request, { required: true, rows: 2, placeholder: 'Summarize what changed since yesterday.' });
  request.setAttribute('aria-label', 'Request');
  const repeat = node('select');
  repeat.setAttribute('aria-label', 'Repeat');
  repeat.append(new Option('Every day', '*'), new Option('Weekdays', '1-5'), new Option('Every week on', 'weekly'));

  const weekday = node('select');
  weekday.setAttribute('aria-label', 'Day');
  weekday.append(...weekdays.map((name, index) => new Option(name, String(index))));
  weekday.value = '1';
  weekday.hidden = true;
  repeat.addEventListener('change', () => { weekday.hidden = repeat.value !== 'weekly'; });

  const time = node('input');
  Object.assign(time, { type: 'time', value: '09:00', required: true });
  time.setAttribute('aria-label', 'Time');
  const zone = node('span', 'cron-zone', zoneName(browserZone));
  const add = node('button', 'dialog-button primary', 'Add');
  add.type = 'submit';
  const when = node('div', 'cron-form-when');
  when.append(repeat, weekday, time, zone, add);
  const error = node('p', 'cron-error');
  error.setAttribute('role', 'status');
  error.hidden = true;
  form.append(request, when, error);

  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (add.disabled) return;
    const message = request.value.trim();
    let validation;
    if (!message) validation = 'Enter a request.';
    else if (new TextEncoder().encode(message).length > maxRequestBytes)
      validation = 'Keep the request to 16,000 bytes or fewer.';
    if (validation) {
      error.textContent = validation;
      error.hidden = false;
      request.focus();
      return;
    }
    const [hour, minute] = time.value.split(':').map(Number);
    const days = repeat.value === 'weekly' ? weekday.value : repeat.value;
    add.disabled = true;
    request.readOnly = true;
    error.hidden = true;
    try {
      await addSchedule({ kind: 'schedule', request: message, cron: `${minute} ${hour} * * ${days}`, timezone: browserZone });
      request.value = '';
    } catch {
      error.textContent = 'Couldn’t add this request.';
      error.hidden = false;
    } finally {
      add.disabled = false;
      request.readOnly = false;
    }
  });
  return form;
}

async function readSchedules(api, bot, signal) {
  const params = new URLSearchParams({ bot, active: '1' });
  const first = await api(`schedules?${params}`, undefined, { signal });
  const { canManage, unscheduledRequests, automations } = first;
  let { before } = first;
  while (before) {
    params.set('before', before);
    const page = await api(`schedules?${params}`, undefined, { signal });
    automations.push(...page.automations);
    before = page.before;
  }
  return { canManage, unscheduledRequests, automations };
}

export function createRecurringRequests(bot, api) {
  const lifetime = new AbortController();
  const { signal } = lifetime;
  const count = document.createTextNode('');
  const list = node('div', 'settings-card cron-list');
  list.tabIndex = -1;
  list.append(node('p', 'cron-empty', 'Loading…'));
  const form = scheduleForm(async (entry) => {
    await api('schedules', { ...entry, botId: bot }, { signal });
    await refresh();
  });
  const remove = async (id) => {
    await api(`schedules?${new URLSearchParams({ bot, id })}`, undefined, { method: 'DELETE', signal });
    await refresh();
  };

  let read;
  let signature;
  async function refresh() {
    if (signal.aborted) return;
    read?.abort();
    const current = read = new AbortController();
    try {
      const data = await readSchedules(api, bot, current.signal);
      if (current.signal.aborted) return;
      const next = JSON.stringify(data);
      if (next === signature) return;
      const { canManage, unscheduledRequests, automations } = data;
      const focused = list.contains(document.activeElement) && document.activeElement.dataset.scheduleId;
      const rows = automations.map((item) => scheduleRow(item, canManage, remove));
      for (const request of unscheduledRequests) {
        const row = node('div', 'cron-row');
        row.append(node('p', 'cron-request', request),
          node('p', 'cron-error', 'This request in the Bot’s configuration isn’t scheduled.'));
        rows.push(row);
      }
      if (!rows.length && !canManage) rows.push(node('p', 'cron-empty', 'No recurring requests.'));
      list.replaceChildren(...rows);
      if (focused) {
        const control = [...list.querySelectorAll('[data-schedule-id]')].find((button) => button.dataset.scheduleId === focused);
        (control ?? list).focus({ preventScroll: true });
      }
      count.data = automations.length ? String(automations.length) : 'None';
      form.hidden = !canManage;
      signature = next;
    } catch {
      if (current.signal.aborted) return;
      signature = undefined;
      count.data = '';
      form.hidden = true;
      list.replaceChildren(node('p', 'cron-empty', 'Couldn’t load recurring requests.'));
    } finally {
      if (read === current) read = null;
    }
  }

  function poll() {
    if (!document.hidden && !read) void refresh();
  }
  const timer = setInterval(poll, 5000);
  document.addEventListener('visibilitychange', poll, { signal });
  poll();
  return {
    count, list, form,
    close() {
      lifetime.abort();
      read?.abort();
      clearInterval(timer);
    },
  };
}
