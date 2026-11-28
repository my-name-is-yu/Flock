import { dumpDom, readAsset, requirePass, withAssetServer } from './dom-harness.mjs';

function fixtures() {
  window.EventSource = undefined;
  const bot = { id: 'release', name: 'Release', model: 'demo', paused: false, canDelete: true, presence: 'idle' };
  const time = Math.floor(Date.now() / 1000) - 600;
  window.reportUnavailableSentinel = 'FLOCK_REPORT_UNAVAILABLE_SYNTHETIC_7C69A4D2';
  const events = [
    { id: 301, kind: 'report', taskId: 17, time, data: {
      report: 'Visible report body', textHtml: '<p>Visible report body</p>', status: 'completed', truncated: false,
      revisionId: 1, resultRunId: 'final',
      run: { link: `${location.origin}/run`, environmentId: 'local', threadId: 'thread', runId: 'launch' },
    } },
    { id: 302, kind: 'completed', taskId: 17, time: time + 1, data: {
      text: 'Visible report body', revisionId: 1,
      run: { environmentId: 'local', threadId: 'thread', runId: 'final' },
    } },
    { id: 303, kind: 'report', taskId: 18, time: time + 2, data: {
      evidenceStatus: 'report_unavailable', report: window.reportUnavailableSentinel,
      textHtml: `<p>${window.reportUnavailableSentinel}</p>`,
    } },
  ];
  window.pauseChanges = [];
  window.sentMessages = [];
  window.sharePausedBot = () => {
    Object.assign(bot, { canDelete: false, paused: true });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.unconfigureBot = (canDelete) => {
    Object.assign(bot, { canDelete, paused: false, model: '' });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.renameBot = (name) => {
    bot.name = name;
    document.dispatchEvent(new Event('visibilitychange'));
  };
  window.fetch = async (path, { body } = {}) => {
    if (path === '/api/bots') return Response.json([bot]);
    if (path === '/api/inbox') return Response.json({ items: [] });
    if (path.startsWith('/api/schedules?')) return Response.json({ active: [], history: [], canManage: bot.canDelete });
    if (path.startsWith('/api/view?')) return Response.json({
      bot, events, olderBefore: 0, presence: 'idle', activeSessions: [], sessionTasks: [], sessionsBefore: 0,
      sessionCursor: 0, waits: [], canDecide: true, composerPerson: 'local-owner',
      limits: { messageBytes: 16000, waitAnswerBytes: 8000, attachments: 4 },
    });
    if (path === '/api/bots/pause') {
      const change = JSON.parse(body);
      pauseChanges.push(change);
      bot.paused = change.paused;
      return Response.json({ paused: bot.paused });
    }
    if (path === '/api/messages') {
      sentMessages.push(JSON.parse(body));
      return Response.json({ messageId: 1 });
    }
    return Response.json({ error: 'unknown_path' }, { status: 404 });
  };
}

async function exercise() {
  const $ = (selector) => document.querySelector(selector);
  const check = (value, message) => { if (!value) throw new Error(message); };
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 50; attempt++) {
      if (predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    check(predicate(), message);
  }
  const settings = $('#detail');
  const confirmation = $('#bot-pause');
  const pauseSetting = () => $('#bot-pause-setting');
  const pauseBanner = $('#composer-banner');
  const resume = $('#composer-banner-action');
  const composer = $('#composer');
  const message = $('#message');
  const menuItems = () => [...document.querySelectorAll('#bot-menu [role="menuitem"]')];

  await waitFor(() => !composer.hidden && !$('#composer-container').hidden && $('#event-302 .event-text') && $('#event-303 .event-text'),
    'The active Bot or rendered reports were missing');
  const visibleReport = $('#event-302 .event-text');
  const unavailableReport = $('#event-303 .event-text');
  check(visibleReport.textContent.includes('Visible report body'), 'An available report body was not displayed');
  check(!document.body.innerText.includes(window.reportUnavailableSentinel) &&
    !unavailableReport.textContent.includes(window.reportUnavailableSentinel),
    'An unavailable report body appeared in the rendered conversation');
  const reportText = $('#event-302 .event-text');
  const point = (type) => {
    const event = new PointerEvent(type, { bubbles: true, cancelable: true, pointerType: 'touch',
      pointerId: 1, isPrimary: true, clientX: 10, clientY: 10 });
    Object.defineProperty(event, 'timeStamp', { value: 100 });
    return reportText.dispatchEvent(event);
  };
  point('pointerdown');
  point('pointerup');
  reportText.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, detail: 1 }));
  const reportTime = $('#event-302 > time');
  check(reportTime && !reportTime.getAttribute('aria-description').includes('#17'),
    'A report exposed its internal Task number to assistive technology');
  const menu = $('.row-menu');
  message.value = 'Draft for after resume';
  message.dispatchEvent(new Event('input'));
  $('#profile-avatar-button').click();
  check(settings.open && pauseSetting().textContent === 'Pause…', 'The owner could not find Pause in settings');
  pauseSetting().click();
  check(confirmation.open && $('#bot-pause-title').textContent === 'Pause Release?', 'Pause did not confirm the Bot');
  $('#bot-pause-cancel').click();
  check(!confirmation.open && !pauseChanges.length, 'Cancelling changed the Bot’s state');
  await new Promise(resolve => setTimeout(resolve, 20));
  pauseSetting().click();
  $('#bot-pause-confirm').click();
  await waitFor(() => !confirmation.open && pauseSetting().textContent === 'Resume…', 'Settings did not switch to Resume');
  check(pauseChanges.length === 1 && pauseChanges[0].botId === 'release' && pauseChanges[0].paused === true,
    'Settings did not pause the selected Bot');
  $('#close-detail').click();
  check($('#profile-status').textContent === 'Paused' && $('.bot-preview').textContent === 'Paused',
    'The conversation header or sidebar did not show Paused');
  check(composer.hidden && message.readOnly && !pauseBanner.hidden && !resume.hidden,
    'The paused Bot still accepted text or hid the owner’s Resume action');
  check($('#composer-banner-title').textContent === 'Release is paused', 'The paused banner did not name the Bot');
  composer.dispatchEvent(new Event('submit', { cancelable: true }));
  check(!sentMessages.length, 'Submitting the hidden form sent a message');
  resume.click();
  await waitFor(() => !composer.hidden && pauseBanner.hidden, 'The composer did not return after Resume');
  check(pauseChanges.length === 2 && pauseChanges[1].paused === false && message.value === 'Draft for after resume',
    'Resume did not preserve the local draft');

  menu.click();
  menuItems().find(item => item.textContent === 'Pause…').click();
  $('#bot-pause-confirm').click();
  await waitFor(() => !confirmation.open && !pauseBanner.hidden, 'The Bot menu could not pause');
  menu.click();
  menuItems().find(item => item.textContent === 'Resume…').click();
  $('#bot-pause-confirm').click();
  await waitFor(() => !confirmation.open && pauseBanner.hidden, 'The Bot menu could not resume');

  window.sharePausedBot();
  await waitFor(() => !pauseBanner.hidden, 'A paused Bot still showed its composer to a member');
  check(resume.hidden, 'A member could resume from the composer');
  menu.click();
  check(menuItems().every(item => !['Pause…', 'Resume…'].includes(item.textContent)), 'A member could manage pause from the menu');
  menu.click();
  $('#profile-avatar-button').click();
  check(settings.open && !pauseSetting(), 'A member could manage pause from settings');
  $('#close-detail').click();

  window.unconfigureBot(false);
  await waitFor(() => pauseBanner.dataset.state === 'setup', 'A Bot without a model still showed its composer');
  check(composer.hidden && resume.hidden && $('#composer-banner-title').textContent === 'Release isn’t set up yet',
    'A member could send to, or set up, a Bot without a model');
  window.unconfigureBot(true);
  await waitFor(() => !resume.hidden, 'The owner could not set up a Bot without a model');
  check(resume.textContent === 'Choose a model', 'The setup action did not say what it does');
  const longName = 'INFRASTRUCTUREMONITORINGSERVICES';
  window.renameBot(longName);
  await waitFor(() => $('#composer-banner-title').textContent === `${longName} isn’t set up yet`, 'The setup banner did not name the Bot');
  pauseBanner.style.width = '296px';
  const bannerBox = pauseBanner.getBoundingClientRect();
  const actionBox = resume.getBoundingClientRect();
  check(actionBox.left >= bannerBox.left && actionBox.right <= bannerBox.right,
    'A long Bot name pushed Choose a model out of a narrow setup banner');
  pauseBanner.style.width = '';
  resume.click();
  check(settings.open && !$('.settings-workspace-form').hidden, 'Choose a model did not open the model editor');
}

const index = (await readAsset('/index.html')).replace('<script type="module" src="/app.js"></script>', `
  <script>(${fixtures})();</script>
  <script type="module">
    import '/app.js';
    try {
      await (${exercise})();
      document.documentElement.dataset.flockResult = 'pass';
    } catch (error) {
      document.documentElement.dataset.flockResult = error.message;
    }
  </script>`);
await withAssetServer({ '/': index }, async (origin) => {
  requirePass(await dumpDom(process.argv[2], `${origin}/?bot=release`), 'The browser did not finish the pause checks');
});
