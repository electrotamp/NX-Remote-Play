'use strict';
const el = (id) => document.getElementById(id);
// Log kept in memory for "Export logs". It never holds a ticket, password, Session ID, token or network address.
const nxLogStart = Date.now(),
  nxLogLines = [];
function nxLog(text) {
  nxLogLines.push(`[${((Date.now() - nxLogStart) / 1000).toFixed(1).padStart(8)}s] ${text}`);
  if (nxLogLines.length > 2000) nxLogLines.splice(0, nxLogLines.length - 2000);
}
function nxLogEnvironment() {
  const c = navigator.connection || {},
    standalone = matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;
  let h264 = 'unknown';
  try {
    h264 = RTCRtpReceiver.getCapabilities('video').codecs.some((x) => /h264/i.test(x.mimeType)) ? 'yes' : 'no';
  } catch {}
  return [
    `Page: ${location.protocol}//(server)${location.pathname}`,
    `Browser: ${navigator.userAgent}`,
    `Language: ${navigator.language}; online ${navigator.onLine}; connection ${c.effectiveType || '?'} ${c.downlink || '?'} Mbps`,
    `Screen: ${screen.width}x${screen.height} at ${window.devicePixelRatio}x; window ${innerWidth}x${innerHeight}`,
    `Secure context: ${window.isSecureContext}; installed app: ${standalone}; H.264 receive: ${h264}`,
  ];
}
nxLog('page opened');
for (const line of nxLogEnvironment()) nxLog(line);
window.addEventListener('error', (event) => nxLog(`error: ${event.message} (${event.filename}:${event.lineno})`));
window.addEventListener('unhandledrejection', (event) =>
  nxLog(`error: unhandled rejection: ${event.reason && event.reason.message ? event.reason.message : event.reason}`),
);
// Saves the log as a text file: the share sheet where the browser offers one for files (phones), else a download.
async function exportLogs() {
  const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\..*/, '').replace('T', '-'),
    name = `NX-Remote-Play-web-log-${stamp}.txt`,
    text =
      `== NX-Remote-Play web player log ==\n` +
      `Created: ${new Date().toString()}\n` +
      `Send this file with a description of what went wrong. It holds no passwords, Session IDs, tickets or addresses.\n\n` +
      nxLogLines.join('\n') +
      '\n';
  const file = new File([text], name, { type: 'text/plain' });
  try {
    if (navigator.canShare && navigator.canShare({ files: [file] })) {
      await navigator.share({ files: [file], title: 'NX Remote Play log' });
      nxLog('log shared');
      return;
    }
  } catch (error) {
    if (error && error.name === 'AbortError') return;
  }
  const link = document.createElement('a');
  link.href = URL.createObjectURL(file);
  link.download = name;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(link.href), 10000);
  nxLog('log downloaded');
}
// Update check: asks GitHub for the newest published release and compares its tag with this page's version.
// The desktop app, the Android app and the Switch app make the same check (COMPATIBILITY.md "Update check").
const NX_WEB_VERSION = '1.0.0-beta.1';
const NX_RELEASES_API = 'https://api.github.com/repos/ElectroTamp/NX-Remote-Play/releases?per_page=10';
let updateRunning = false,
  updateAnnounced = false;
function parseVersion(text) {
  let s = String(text).trim().replace(/^[vV]/, '').split('+')[0];
  let pre = [];
  const dash = s.indexOf('-');
  if (dash >= 0) {
    pre = s.slice(dash + 1).split('.');
    s = s.slice(0, dash);
  }
  const core = s.split('.');
  if (core.length !== 3 || !core.every((p) => /^\d+$/.test(p)) || pre.some((p) => p === '')) return null;
  return { core: core.map(Number), pre };
}
// Semantic version order: 1 newer, -1 older, 0 equal, null when either side is not a version.
function compareVersions(a, b) {
  const x = parseVersion(a),
    y = parseVersion(b);
  if (!x || !y) return null;
  for (let i = 0; i < 3; i++) if (x.core[i] !== y.core[i]) return x.core[i] > y.core[i] ? 1 : -1;
  if (!x.pre.length || !y.pre.length) return x.pre.length === y.pre.length ? 0 : x.pre.length ? -1 : 1;
  for (let i = 0; i < Math.min(x.pre.length, y.pre.length); i++) {
    const p = x.pre[i],
      q = y.pre[i],
      np = /^\d+$/.test(p),
      nq = /^\d+$/.test(q);
    if (p === q) continue;
    if (np && nq) return Number(p) > Number(q) ? 1 : -1;
    if (np || nq) return np ? -1 : 1;
    return p > q ? 1 : -1;
  }
  return Math.sign(x.pre.length - y.pre.length);
}
function showUpdateState(text) {
  for (const node of document.querySelectorAll('[data-update-status]')) node.textContent = text;
}
async function checkForUpdates() {
  if (updateRunning) return;
  updateRunning = true;
  showUpdateState('Checking...');
  try {
    const response = await fetch(NX_RELEASES_API, { headers: { Accept: 'application/vnd.github+json' }, cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    let newest = '';
    for (const release of await response.json()) {
      const tag = release && !release.draft ? String(release.tag_name || '') : '';
      if (compareVersions(tag, '0.0.0') !== null && (!newest || compareVersions(tag, newest) > 0)) newest = tag;
    }
    if (newest && compareVersions(newest, NX_WEB_VERSION) > 0) {
      const version = newest.replace(/^[vV]/, '');
      showUpdateState(`Update available: ${version}`);
      nxLog(`update check: ${version} is available`);
      if (!updateAnnounced && el('player').hidden) {
        updateAnnounced = true;
        el('updateText').textContent = `Version ${version} is out. This page is ${NX_WEB_VERSION}.`;
        el('updateDialog').hidden = false;
        el('updateOpen').focus();
      }
    } else {
      showUpdateState('Up to date');
      nxLog('update check: up to date');
    }
  } catch (error) {
    showUpdateState('Could not check');
    nxLog(`update check failed: ${error && error.message ? error.message : error}`);
  } finally {
    updateRunning = false;
  }
}
// A shared link fills the session ID only. Passwords stay out of URLs/history.
const linkSession = new URLSearchParams(location.search).get('session');
if (linkSession && /^[A-Za-z0-9]{5}$/.test(linkSession))
  el('sessionId').value = linkSession.toUpperCase();
// A session link from the desktop app carries the session's token in the URL fragment (never sent to a server in a request line, not in
// Referer, removed from the address bar at once). It replaces the password for this join; it stays valid until the host's USB session ends.
let linkToken = (/^#t=([0-9A-Fa-f]{64})$/.exec(location.hash) || [])[1] || null;
if (location.hash) {
  try {
    history.replaceState(null, '', location.pathname + location.search);
  } catch {}
}
if (linkToken) el('password').required = false;
let pc = null,
  ws = null,
  epoch = 0,
  frame = null,
  frameSequence = 0,
  frameOffset = 0,
  homeUrl = null,
  gamePending = false;
let candidateBacklog = [],
  pendingHomeUrl = null;
// What the connection attempt found, shown when it ends: tells a blocked network path from other failures.
function pathNote(note) {
  return note
    ? ` Network path: ICE ${note.state || 'never started'}; this browser offered ${[...note.local].join(', ') || 'no candidates'}; the host offered ${[...note.remote].join(', ') || 'no candidates'}.`
    : '';
}
// Measurement readout drawn over the video. One button/H key hides or shows ALL of it; the button is never hidden.
let statsTimer = null,
  statsPrev = null,
  statsMode,
  lastSeq = null;
const homeStats = {
  received: 0,
  shown: 0,
  hostSkipped: 0,
  busyDropped: 0,
  broken: 0,
  lastShownAt: 0,
};
// Stats drawn over the picture: 0 off, 1 frame rate, 2 frame rate and details.
statsMode = 0;
try {
  statsMode = Math.min(2, Math.max(0, Number(localStorage.getItem('nxStatsMode')) || 0));
} catch {}
function applyStatsVisibility() {
  el('stats').hidden = statsMode < 2;
  document.body.classList.toggle('show-fps', statsMode >= 1);
  el('statsToggle').lastElementChild.textContent = ['Off', 'FPS', 'Details'][statsMode];
  try {
    localStorage.setItem('nxStatsMode', String(statsMode));
  } catch {}
}
function toggleStats() {
  statsMode = (statsMode + 1) % 3;
  applyStatsVisibility();
}
el('statsToggle').addEventListener('click', toggleStats);
el('statsToggle').addEventListener('pointerdown', (event) => event.stopPropagation());
applyStatsVisibility();
function stopStats() {
  if (statsTimer) {
    clearInterval(statsTimer);
    statsTimer = null;
  }
  statsPrev = null;
  el('stats').textContent = '';
  publishStats(NaN, 0, 0, NaN);
}
function publishStats(fps, width, height, rtt) {
  const text = Number.isFinite(fps) ? `${Math.round(fps)} FPS` : '– FPS';
  el('fpsPill').textContent = text;
  el('statFps').textContent = text;
  const size = width && height ? `${width}×${height}` : '';
  el('statRes').textContent = size;
  el('statRes').hidden = !size;
  const lag = Number.isFinite(rtt) ? `${Math.round(rtt)} ms` : '';
  el('statRtt').textContent = lag;
  el('statRtt').hidden = !lag;
}
async function sampleStats(connection) {
  if (connection !== pc) return;
  let video = null,
    pair = null,
    audio = null;
  (await connection.getStats()).forEach((report) => {
    if (report.type === 'inbound-rtp' && report.kind === 'video') video = report;
    if (report.type === 'inbound-rtp' && report.kind === 'audio') audio = report;
    if (report.type === 'candidate-pair' && report.state === 'succeeded' && report.nominated)
      pair = report;
  });
  const now = performance.now(),
    lines = [],
    home = { ...homeStats };
  if (video) {
    if (statsPrev?.video) {
      const dt = (now - statsPrev.at) / 1000,
        p = statsPrev.video,
        fps = (video.framesDecoded - p.framesDecoded) / dt;
      const emitted = video.jitterBufferEmittedCount - p.jitterBufferEmittedCount,
        jb = emitted > 0 ? ((video.jitterBufferDelay - p.jitterBufferDelay) / emitted) * 1000 : NaN;
      const decoded = video.framesDecoded - p.framesDecoded,
        decode = decoded > 0 ? ((video.totalDecodeTime - p.totalDecodeTime) / decoded) * 1000 : NaN;
      const received =
        Number.isFinite(video.framesReceived) && Number.isFinite(p.framesReceived)
          ? (video.framesReceived - p.framesReceived) / dt
          : fps;
      publishStats(
        received,
        video.frameWidth,
        video.frameHeight,
        pair && pair.currentRoundTripTime != null ? pair.currentRoundTripTime * 1000 : NaN,
      );
      lines.push(
        `Video ${fps.toFixed(1)} fps ${video.frameWidth || '?'}x${video.frameHeight || '?'}  dropped ${(video.framesDropped || 0) - (p.framesDropped || 0)}/s (${video.framesDropped || 0} total)  lost ${(video.packetsLost || 0) - (p.packetsLost || 0)}/s`,
      );
      lines.push(
        `Jitter buffer ${Number.isFinite(jb) ? jb.toFixed(0) : '–'} ms (target 0)  decode ${Number.isFinite(decode) ? decode.toFixed(1) : '–'} ms  freezes ${video.freezeCount || 0}  RTT ${pair && pair.currentRoundTripTime != null ? (pair.currentRoundTripTime * 1000).toFixed(0) : '–'} ms`,
      );
      reportWebStats(
        now,
        dt,
        video,
        p,
        audio,
        statsPrev.audio,
        pair,
        fps,
        received,
        jb,
        decode,
        decoded,
        emitted,
      );
    } else lines.push('Video: measuring…');
  } else lines.push('Video: no stream yet');
  if (statsPrev?.home && (home.received || home.shown || now - homeStats.lastShownAt < 5000)) {
    const dt = (now - statsPrev.at) / 1000,
      p = statsPrev.home;
    lines.push(
      `HOME ${((home.shown - p.shown) / dt).toFixed(1)} shown/s  received ${((home.received - p.received) / dt).toFixed(1)}/s  skipped by host ${home.hostSkipped - p.hostSkipped}  busy-dropped ${home.busyDropped - p.busyDropped}  broken ${home.broken - p.broken}`,
    );
  }
  el('stats').textContent = lines.join('\n');
  statsPrev = { at: now, video, audio, home };
}
// Measurement only: the numbers the readout above is built from, sent to the host once a second over the "home" data channel as
// "w1,<27 numbers>" (empty = this browser does not provide it). The host writes them to web-browser-*.csv next to its own delivery
// numbers so a latency build-up can be read from both ends. Nothing else is sent; an older host ignores the message.
let homeChannel = null,
  webStatsTick = 0;
function reportWebStats(
  now,
  dt,
  video,
  p,
  audio,
  pa,
  pair,
  fps,
  received,
  jb,
  decode,
  decoded,
  emitted,
) {
  if (++webStatsTick % 5 === 0)
    nxLog(
      `stream: ${Math.round(fps || 0)} fps, ${video.frameWidth}x${video.frameHeight}, dropped ${video.framesDropped}, lost ${video.packetsLost}, freezes ${video.freezeCount}, rtt ${pair && pair.currentRoundTripTime != null ? Math.round(pair.currentRoundTripTime * 1000) : '?'} ms, decode ${Number.isFinite(decode) ? Math.round(decode * 10) / 10 : '?'} ms`,
    );
  const channel = homeChannel;
  if (!channel || channel.readyState !== 'open' || channel.bufferedAmount > 2048) return;
  const d = (a, b, k) =>
    a && b && Number.isFinite(a[k]) && Number.isFinite(b[k]) ? a[k] - b[k] : NaN;
  const ifdSum = d(video, p, 'totalInterFrameDelay'),
    ifdSq = d(video, p, 'totalSquaredInterFrameDelay');
  const ifdMean = decoded > 0 && Number.isFinite(ifdSum) ? (ifdSum / decoded) * 1000 : NaN;
  const ifdStd =
    decoded > 0 && Number.isFinite(ifdSq) && Number.isFinite(ifdMean)
      ? Math.sqrt(Math.max(0, (ifdSq / decoded) * 1e6 - ifdMean * ifdMean))
      : NaN;
  const target = emitted > 0 ? (d(video, p, 'jitterBufferTargetDelay') / emitted) * 1000 : NaN;
  const aEmit = d(audio, pa, 'jitterBufferEmittedCount'),
    aJitter = aEmit > 0 ? (d(audio, pa, 'jitterBufferDelay') / aEmit) * 1000 : NaN;
  const rxPackets = d(video, p, 'packetsReceived'),
    rxKbps = (d(video, p, 'bytesReceived') * 8) / dt / 1000;
  const fields = [
    now,
    fps,
    received,
    video.frameWidth,
    video.frameHeight,
    jb,
    target,
    decode,
    ifdMean,
    ifdStd,
    video.framesDropped,
    video.packetsLost,
    rxPackets,
    rxKbps,
    video.nackCount,
    video.pliCount,
    video.freezeCount,
    Number.isFinite(video.totalFreezesDuration) ? video.totalFreezesDuration * 1000 : NaN,
    pair && pair.currentRoundTripTime != null ? pair.currentRoundTripTime * 1000 : NaN,
    aJitter,
    d(audio, pa, 'concealedSamples'),
    d(audio, pa, 'removedSamplesForAcceleration'),
    d(audio, pa, 'insertedSamplesForDeceleration'),
    audio ? audio.packetsLost : NaN,
    document.hidden ? 1 : 0,
    emitted > 0 ? (d(video, p, 'jitterBufferMinimumDelay') / emitted) * 1000 : NaN,
    aEmit > 0 ? (d(audio, pa, 'jitterBufferTargetDelay') / aEmit) * 1000 : NaN,
  ];
  try {
    channel.send(
      'w1,' +
        fields.map((v) => (Number.isFinite(v) ? String(Math.round(v * 100) / 100) : '')).join(','),
    );
  } catch {}
}
function startStats(connection) {
  stopStats();
  statsTimer = setInterval(() => {
    sampleStats(connection).catch(() => {});
  }, 1000);
}
let input = null,
  grant = 0n,
  inputSequence = 0,
  touchEnabled = false,
  touchPointer = null;
const pressed = new Map();
// On-screen sticks: { pointer, x, y } with x/y in -1..1 (y up), or null when not held.
const touchSticks = [null, null];
let screenTouch = null;
// Per-client "disconnect controller": off = this page sends no pad state, keys or HOME, so it takes no player place; touch still works.
let padEnabled = true;
try {
  padEnabled = localStorage.getItem('nxPadEnabled') !== '0';
} catch {}
function renderPadToggle() {
  const b = el('padToggle');
  if (!b) return;
  b.setAttribute('aria-pressed', padEnabled ? 'true' : 'false');
  b.querySelector('em').textContent = padEnabled ? 'On' : 'Off';
}
el('padToggle')?.addEventListener('click', () => {
  padEnabled = !padEnabled;
  try {
    localStorage.setItem('nxPadEnabled', padEnabled ? '1' : '0');
  } catch {}
  releaseControls();
  sendControls(true);
  renderPadToggle();
});
renderPadToggle();
function releaseControls() {
  pressed.clear();
  touchSticks.fill(null);
  document.querySelectorAll('#controls .knob').forEach((knob) => (knob.style.transform = ''));
  screenTouch = null;
  touchPointer = null;
  document.querySelectorAll('.pressed').forEach((button) => button.classList.remove('pressed'));
}
function inputFrame(owner, sequence, state) {
  const bytes = new ArrayBuffer(48),
    view = new DataView(bytes);
  view.setUint32(0, 0x3149584e, true);
  view.setUint16(4, 1, true);
  view.setUint16(6, 48, true);
  view.setBigUint64(8, owner, true);
  view.setUint32(16, sequence, true);
  view.setBigUint64(20, BigInt(state.keys & 65535), true);
  state.axes.forEach((axis, index) =>
    view.setInt16(28 + index * 2, Math.max(-32768, Math.min(32767, axis)), true),
  );
  view.setUint8(36, state.home ? 1 : 0);
  view.setUint8(37, state.touch ? 1 : 0);
  view.setUint16(38, state.touch?.x || 0, true);
  view.setUint16(40, state.touch?.y || 0, true);
  return bytes;
}
function sendControls(neutral = false) {
  const pad = neutral || !padEnabled ? null : controllerInput.poll();
  if (!grant || input?.readyState !== 'open' || input.bufferedAmount > 4096) return;
  const state = { keys: 0, axes: [0, 0, 0, 0], home: false, touch: neutral ? null : screenTouch };
  if (!neutral) {
    if (padEnabled) {
      for (const value of pressed.values()) {
        if (value === 'home') state.home = true;
        else state.keys |= 1 << value;
      }
      if (pad) controllerInput.apply(pad, state);
      // A held on-screen stick wins over the same physical stick.
      touchSticks.forEach((stick, index) => {
        if (!stick) return;
        state.axes[index * 2] = Math.round(stick.x * 32767);
        state.axes[index * 2 + 1] = Math.round(stick.y * 32767);
      });
    }
  }
  try {
    input.send(inputFrame(grant, inputSequence++ >>> 0, state));
  } catch {
    grant = 0n;
    releaseControls();
  }
}
setInterval(() => {
  if (!document.hidden) sendControls();
}, 50);
controllerInput.init(() => {
  releaseControls();
  sendControls(true);
});
window.addEventListener('gamepadconnected', () => sendControls());
window.addEventListener('gamepaddisconnected', () => sendControls());
document.querySelectorAll('#controls [data-key],#controls [data-home]').forEach((button) => {
  button.addEventListener('pointerdown', (event) => {
    if (!grant) return;
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    pressed.set(event.pointerId, button.dataset.home ? 'home' : Number(button.dataset.key));
    button.classList.add('pressed');
    sendControls();
  });
  const up = (event) => {
    pressed.delete(event.pointerId);
    button.classList.remove('pressed');
    sendControls();
  };
  button.addEventListener('pointerup', up);
  button.addEventListener('pointercancel', up);
  button.addEventListener('lostpointercapture', up);
});
document.querySelectorAll('#controls .stick').forEach((base) => {
  const index = Number(base.dataset.stick),
    knob = base.querySelector('.knob');
  const move = (event) => {
    const rect = base.getBoundingClientRect(),
      radius = rect.width / 2;
    let x = (event.clientX - rect.left - radius) / radius,
      y = (event.clientY - rect.top - radius) / radius;
    const length = Math.hypot(x, y);
    if (length > 1) {
      x /= length;
      y /= length;
    }
    knob.style.transform = `translate(${x * radius * 0.6}px, ${y * radius * 0.6}px)`;
    // Small dead zone so resting a thumb on the centre sends neutral.
    const live = Math.hypot(x, y) < 0.12 ? 0 : 1;
    touchSticks[index] = { pointer: event.pointerId, x: x * live, y: -y * live };
    sendControls();
  };
  base.addEventListener('pointerdown', (event) => {
    if (!grant || touchSticks[index]) return;
    event.preventDefault();
    base.setPointerCapture(event.pointerId);
    base.classList.add('pressed');
    move(event);
  });
  base.addEventListener('pointermove', (event) => {
    if (touchSticks[index]?.pointer === event.pointerId) move(event);
  });
  const up = (event) => {
    if (touchSticks[index]?.pointer !== event.pointerId) return;
    touchSticks[index] = null;
    knob.style.transform = '';
    base.classList.remove('pressed');
    sendControls();
  };
  base.addEventListener('pointerup', up);
  base.addEventListener('pointercancel', up);
  base.addEventListener('lostpointercapture', up);
});
function touchPoint(event) {
  const rect = el('viewport').getBoundingClientRect(),
    width = Math.min(rect.width, (rect.height * 16) / 9),
    height = (width * 9) / 16;
  const x = event.clientX - rect.left - (rect.width - width) / 2,
    y = event.clientY - rect.top - (rect.height - height) / 2;
  if (x < 0 || y < 0 || x >= width || y >= height) return null;
  return {
    x: Math.min(1279, Math.floor((x / width) * 1280)),
    y: Math.min(719, Math.floor((y / height) * 720)),
  };
}
el('viewport').addEventListener('pointerdown', (event) => {
  if (!grant || !touchEnabled || touchPointer !== null) return;
  const point = touchPoint(event);
  if (!point) return;
  event.preventDefault();
  touchPointer = event.pointerId;
  screenTouch = point;
  el('viewport').setPointerCapture(event.pointerId);
  sendControls();
});
el('viewport').addEventListener('pointermove', (event) => {
  if (event.pointerId !== touchPointer) return;
  screenTouch = touchPoint(event);
  sendControls();
});
for (const name of ['pointerup', 'pointercancel', 'lostpointercapture'])
  el('viewport').addEventListener(name, (event) => {
    if (event.pointerId !== touchPointer) return;
    screenTouch = null;
    touchPointer = null;
    sendControls();
  });
el('touch').addEventListener('click', () => {
  touchEnabled = !touchEnabled;
  el('viewport').classList.toggle('touch-enabled', touchEnabled);
  el('touch').setAttribute('aria-pressed', touchEnabled);
  el('touch').querySelector('em').textContent = touchEnabled ? 'On' : 'Off';
  screenTouch = null;
  sendControls();
});
window.addEventListener('blur', () => {
  releaseControls();
  sendControls(true);
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    releaseControls();
    sendControls(true);
  }
});
function status(text) {
  el('status').textContent = text;
  nxLog('status: ' + text);
}
function connectedStatus() {
  status(grant ? 'Connected • You have control' : 'Connected • View only');
  setState('Connected', true);
}
function setState(text, ok) {
  nxLog('state: ' + text);
  const chip = el('state');
  chip.textContent = text;
  chip.classList.toggle('ok', !!ok);
}
function receiveControlNotification(text) {
  if (typeof text !== 'string' || !/^control:[0-9]{1,20}$/.test(text)) return;
  const next = BigInt(text.slice(8));
  if (next > 0xffffffffffffffffn || next === grant || (next !== 0n && next < grant)) return;
  releaseControls();
  grant = next;
  inputSequence = 0;
  el('controls').hidden = !grant;
  if (grant) connectedStatus();
  else status('Restoring controls automatically...');
  sendControls(true);
}
function clear() {
  sendControls(true);
  grant = 0n;
  input = null;
  releaseControls();
  el('controls').hidden = true;
  stopStats();
  homeChannel = null;
  lastSeq = null;
  epoch++;
  if (ws) {
    ws.onclose = null;
    ws.close();
    ws = null;
  }
  if (pc) {
    pc.close();
    pc = null;
  }
  candidateBacklog = [];
  frame = null;
  frameOffset = 0;
  gamePending = false;
  el('video').srcObject = null;
  el('home').hidden = true;
  el('home').onload = null;
  el('home').onerror = null;
  el('home').removeAttribute('src');
  if (homeUrl) {
    URL.revokeObjectURL(homeUrl);
    homeUrl = null;
  }
  if (pendingHomeUrl) {
    URL.revokeObjectURL(pendingHomeUrl);
    pendingHomeUrl = null;
  }
  el('join').hidden = false;
  el('player').hidden = true;
  el('connect').disabled = false;
  el('video').muted = true;
  soundLabel();
}
function signal(type, payload) {
  if (ws?.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ session: '', type, payload }));
}
function receiveHome(event) {
  if (typeof event.data === 'string') {
    if (event.data === 'game') {
      // An image load can finish after the game notification and cover live video.
      // Keep the displayed HOME image until a decoded game frame, but retire pending loads.
      frame = null;
      frameOffset = 0;
      if (pendingHomeUrl) {
        URL.revokeObjectURL(pendingHomeUrl);
        pendingHomeUrl = null;
      }
      gamePending = true;
    }
    return;
  }
  const data = event.data;
  if (!(data instanceof ArrayBuffer) || data.byteLength < 13) return;
  const header = new DataView(data),
    sequence = header.getUint32(0, true),
    total = header.getUint32(4, true),
    offset = header.getUint32(8, true);
  if (total < 1 || total > 0x54000 || offset > total || data.byteLength - 12 > total - offset) {
    frame = null;
    homeStats.broken++;
    return;
  }
  if (offset === 0) {
    if (lastSeq !== null && sequence > lastSeq + 1) homeStats.hostSkipped += sequence - lastSeq - 1;
    lastSeq = sequence;
    frameSequence = sequence;
    frame = new Uint8Array(total);
    frameOffset = 0;
  }
  if (!frame || sequence !== frameSequence || total !== frame.length || offset !== frameOffset) {
    if (frame) homeStats.broken++;
    frame = null;
    return;
  }
  frame.set(new Uint8Array(data, 12), offset);
  frameOffset += data.byteLength - 12;
  if (frameOffset === total) {
    const complete = frame;
    frame = null;
    homeStats.received++;
    if (pendingHomeUrl) {
      homeStats.busyDropped++;
      return;
    }
    const current = epoch,
      url = URL.createObjectURL(new Blob([complete], { type: 'image/jpeg' }));
    pendingHomeUrl = url;
    el('home').onload = () => {
      if (current !== epoch || pendingHomeUrl !== url) return;
      homeStats.shown++;
      homeStats.lastShownAt = performance.now();
      if (homeUrl) URL.revokeObjectURL(homeUrl);
      homeUrl = url;
      pendingHomeUrl = null;
      el('home').hidden = false;
      gamePending = false;
      connectedStatus();
    };
    el('home').onerror = () => {
      if (pendingHomeUrl === url) {
        URL.revokeObjectURL(url);
        pendingHomeUrl = null;
      }
    };
    el('home').src = url;
  }
}
function videoFrame() {
  if (gamePending || el('home').hidden) {
    el('home').hidden = true;
    gamePending = false;
    connectedStatus();
  }
  if (el('video').requestVideoFrameCallback) el('video').requestVideoFrameCallback(videoFrame);
}
if (el('video').requestVideoFrameCallback) el('video').requestVideoFrameCallback(videoFrame);
el('video').addEventListener('playing', () => {
  if (gamePending || el('home').hidden) {
    el('home').hidden = true;
    gamePending = false;
    connectedStatus();
  }
});
el('joinForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  clear();
  const current = epoch;
  el('connect').disabled = true;
  status('Connecting…');
  const id = el('sessionId').value.trim().toUpperCase(),
    password = el('password').value.trim().toUpperCase();
  const token = linkToken;
  linkToken = null;
  el('password').required = true; // one automatic attempt per page load, never a retry loop
  try {
    if (!/^[A-Z0-9]{5}$/.test(id) || (!token && !/^[A-Z0-9]{5}$/.test(password)))
      throw new Error('Enter the five-character ID and password.');
    const response = await fetch('/v1/rtc/session', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(token ? { id, token } : { id, password }),
      cache: 'no-store',
      signal: AbortSignal.timeout(7000),
    });
    if (current !== epoch) return;
    if (!response.ok)
      throw new Error(
        token && response.status === 404
          ? 'This link is no longer valid: the host session it belonged to has ended. Ask for the current link, or enter the ID and password.'
          : response.status === 409
            ? 'The bridge is offline or already has a viewer.'
            : 'No active bridge is available with that ID and password. Check the host and Switch connection.',
      );
    const ticket = await response.json();
    if (!/^[0-9A-F]{64}$/.test(ticket.ticket)) throw new Error('Invalid connection response.');
    if (ticket.iceServers && (!Array.isArray(ticket.iceServers) || ticket.iceServers.length > 8))
      throw new Error('Invalid connection servers.');
    if (ticket.relayOnly) throw new Error('Relay connections are unsupported.');
    for (const server of ticket.iceServers || []) {
      if (
        !Array.isArray(server.urls) ||
        server.urls.length < 1 ||
        server.urls.length > 8 ||
        server.urls.some(
          (url) => typeof url !== 'string' || url.length > 256 || !url.startsWith('stun:'),
        ) ||
        server.username ||
        server.credential
      )
        throw new Error('Only direct connection discovery is supported.');
    }
    pc = new RTCPeerConnection({ iceServers: ticket.iceServers || [], iceTransportPolicy: 'all' });
    const connection = pc;
    const note = { state: '', local: new Set(), remote: new Set() };
    connection.oniceconnectionstatechange = () => {
      if (current === epoch) {
        note.state = connection.iceConnectionState;
        nxLog('ice: ' + note.state);
      }
    };
    const video = connection.addTransceiver('video', { direction: 'recvonly' }),
      audio = connection.addTransceiver('audio', { direction: 'recvonly' });
    if (video.setCodecPreferences) {
      const formats = RTCRtpReceiver.getCapabilities('video').codecs.filter(
        (codec) => codec.mimeType.toLowerCase() === 'video/h264',
      );
      if (formats.length) video.setCodecPreferences(formats);
    }
    if (audio.setCodecPreferences) {
      const formats = RTCRtpReceiver.getCapabilities('audio').codecs.filter(
        (codec) => codec.mimeType.toLowerCase() === 'audio/opus',
      );
      if (formats.length) audio.setCodecPreferences(formats);
    }
    const home = connection.createDataChannel('home', { ordered: true, maxRetransmits: 0 });
    home.binaryType = 'arraybuffer';
    home.onmessage = receiveHome;
    homeChannel = home;
    input = connection.createDataChannel('input', { ordered: false, maxRetransmits: 0 });
    input.binaryType = 'arraybuffer';
    input.onmessage = (event) => {
      if (current === epoch) receiveControlNotification(event.data);
    };
    input.onclose = () => {
      if (current === epoch) {
        grant = 0n;
        releaseControls();
        el('controls').hidden = true;
      }
    };
    connection.ontrack = (event) => {
      if (current !== epoch) return;
      // Ask the browser for the smallest video receive buffer; the readout shows what it actually uses.
      if (event.track.kind === 'video' && event.receiver) {
        try {
          event.receiver.jitterBufferTarget = 0;
        } catch {
          try {
            event.receiver.playoutDelayHint = 0;
          } catch {}
        }
      }
      const stream = el('video').srcObject || new MediaStream();
      stream.addTrack(event.track);
      el('video').srcObject = stream;
      el('video')
        .play()
        .catch(() => {});
    };
    let offerSent = false,
      outgoingCandidates = [];
    connection.onicecandidate = (event) => {
      if (event.candidate && current === epoch) {
        note.local.add(event.candidate.type || '?');
        const candidate = JSON.stringify(event.candidate.toJSON());
        if (offerSent) signal('candidate', candidate);
        else if (outgoingCandidates.length < 64) outgoingCandidates.push(candidate);
      }
    };
    connection.onconnectionstatechange = () => {
      if (current !== epoch) return;
      nxLog('connection: ' + connection.connectionState);
      if (connection.connectionState === 'connected') {
        startStats(connection);
        el('join').hidden = true;
        el('player').hidden = false;
        el('password').value = '';
        setState('Connecting', false);
        status('Connected • Waiting for video');
      }
      if (['failed', 'closed'].includes(connection.connectionState)) {
        clear();
        status('Connection ended. You can reconnect.' + pathNote(note));
      }
    };
    ws = new WebSocket(
      `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/v1/rtc/socket?ticket=${ticket.ticket}`,
    );
    let signalingOpened = false;
    ws.onopen = () => {
      signalingOpened = true;
      nxLog('signaling opened');
    };
    ws.onclose = (event) => {
      nxLog(`signaling closed (code ${event.code})`);
      if (current === epoch) {
        clear();
        status(
          signalingOpened
            ? `The browser connection closed during setup or playback (code ${event.code}). You can reconnect.` +
                pathNote(note)
            : `The browser could not open the connection (code ${event.code}). Check the test certificate warning and network.`,
        );
      }
    };
    ws.onmessage = async (event) => {
      if (current !== epoch) return;
      try {
        const message = JSON.parse(event.data);
        if (message.type === 'ready') {
          const offer = await connection.createOffer();
          await connection.setLocalDescription(offer);
          signal('offer', connection.localDescription.sdp);
          offerSent = true;
          for (const candidate of outgoingCandidates) signal('candidate', candidate);
          outgoingCandidates = [];
        }
        if (message.type === 'answer') {
          await connection.setRemoteDescription({ type: 'answer', sdp: message.payload });
          for (const candidate of candidateBacklog) await connection.addIceCandidate(candidate);
          candidateBacklog = [];
        }
        if (message.type === 'candidate') {
          const candidate = JSON.parse(message.payload);
          note.remote.add((/ typ (\w+)/.exec(candidate.candidate || '') || [])[1] || '?');
          if (connection.remoteDescription) await connection.addIceCandidate(candidate);
          else {
            if (candidateBacklog.length >= 64) throw new Error('Too many connection candidates.');
            candidateBacklog.push(candidate);
          }
        }
        if (message.type === 'close') {
          clear();
          status('Connection ended.');
        }
      } catch {
        clear();
        status('Could not establish the video connection.');
      }
    };
  } catch (error) {
    if (current === epoch) {
      clear();
      status(error.message || 'Connection failed.');
    }
  }
});
el('exportLogs').addEventListener('click', exportLogs);
el('updateJoin').addEventListener('click', checkForUpdates);
el('updateCheck').addEventListener('click', checkForUpdates);
for (const id of ['updateOpen', 'updateLater']) el(id).addEventListener('click', () => (el('updateDialog').hidden = true));
el('updateDialog').addEventListener('keydown', (event) => {
  if (event.key === 'Escape') el('updateDialog').hidden = true;
});
checkForUpdates();
el('exportLogsJoin').addEventListener('click', exportLogs);
el('disconnect').addEventListener('click', () => {
  signal('close', '');
  clear();
  status('Ready');
});
el('sound').addEventListener('click', () => {
  el('video').muted = !el('video').muted;
  soundLabel();
  void el('video')
    .play()
    .catch(() => {});
});
function soundLabel() {
  el('sound').lastElementChild.textContent = el('video').muted ? 'Off' : 'On';
}
el('fullscreen').addEventListener('click', () => enterFullscreen());
window.addEventListener('pagehide', clear);

// ---- full screen, install as an app, screen wake ----
const standalone = () =>
  navigator.standalone === true ||
  matchMedia('(display-mode: standalone)').matches ||
  matchMedia('(display-mode: fullscreen)').matches;
const iosDevice = () =>
  /iPhone|iPad|iPod/.test(navigator.userAgent) ||
  (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
const fullscreenElement = () => document.fullscreenElement || document.webkitFullscreenElement;
const canFullscreen = () => !!(document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen);
let enteredFullscreen = false, // the page is in browser full screen because we asked
  restoreUiOnExit = false, // the Fullscreen button hid a visible interface: bring it back when full screen ends
  fullscreenDeclined = false; // the user left full screen in landscape: do not start it again until the phone is turned
// Full screen = the picture fills the page (Hide UI: pads and the circle menu stay on it) plus the browser's own full screen where it
// exists (Android, desktop). The page itself goes full screen, not only the picture: an element in full screen hides everything
// outside it, so the pads and the circle menu would vanish. iPhone Safari has no element full screen and the video's own player
// replaces the page, so it only gets the first part; its bars go away when the page is opened from the Home Screen.
function exitFullscreen() {
  (document.exitFullscreen || document.webkitExitFullscreen)?.call(document);
}
// Must run inside a tap or click. lockLandscape turns a phone held upright sideways; after a rotation it stays free to turn back.
function requestPageFullscreen(lockLandscape) {
  const page = document.documentElement,
    request = page.requestFullscreen || page.webkitRequestFullscreen;
  if (!request) return;
  Promise.resolve(request.call(page))
    .then(() => {
      enteredFullscreen = true;
      try {
        if (lockLandscape) screen.orientation?.lock?.('landscape')?.catch(() => {});
      } catch {}
    })
    .catch(() => (restoreUiOnExit = false));
}
function enterFullscreen() {
  if (fullscreenElement()) {
    exitFullscreen();
    return;
  }
  restoreUiOnExit = !ui.hidden && canFullscreen();
  ui.hidden = true;
  applyUi();
  requestPageFullscreen(true);
}
// Browsers only allow full screen from a tap or while one is still fresh (about 5 s), so turning the phone cannot start it on its own:
// the first tap after turning does (a pad button, the circle, the picture), and a hint says so. An installed app is already full screen.
function wantsFullscreen() {
  return (
    ui.hidden &&
    inPlayer() &&
    rotateMq.matches &&
    !fullscreenDeclined &&
    !fullscreenElement() &&
    !standalone() &&
    canFullscreen()
  );
}
let hintTimer = 0;
function hintFullscreen() {
  const hint = el('fsHint');
  hint.hidden = !wantsFullscreen();
  clearTimeout(hintTimer);
  if (!hint.hidden) hintTimer = setTimeout(() => (hint.hidden = true), 4000);
}
document.addEventListener(
  'pointerup',
  () => {
    el('fsHint').hidden = true;
    if (wantsFullscreen()) requestPageFullscreen(false);
  },
  true,
);
function fullscreenChanged() {
  if (fullscreenElement()) return;
  try {
    screen.orientation?.unlock?.();
  } catch {}
  if (!enteredFullscreen) return;
  enteredFullscreen = false;
  fullscreenDeclined = rotateMq.matches;
  if (!restoreUiOnExit) return;
  restoreUiOnExit = false;
  ui.hidden = false;
  applyUi();
}
document.addEventListener('fullscreenchange', fullscreenChanged);
document.addEventListener('webkitfullscreenchange', fullscreenChanged);
let installPrompt = null;
function showInstall() {
  const can = (!!installPrompt || iosDevice()) && !standalone(); // iOS never offers an install prompt: the button explains the Share menu steps
  for (const id of ['install', 'installJoin']) el(id).hidden = !can;
  el('iosHint').hidden = !(iosDevice() && !standalone());
}
window.addEventListener('beforeinstallprompt', (event) => {
  event.preventDefault();
  installPrompt = event;
  showInstall();
});
window.addEventListener('appinstalled', () => {
  installPrompt = null;
  showInstall();
});
for (const id of ['install', 'installJoin'])
  el(id).addEventListener('click', async () => {
    const prompt = installPrompt;
    if (!prompt) {
      if (iosDevice()) {
        el('iosHint').hidden = false;
        status('To install: tap Share, then Add to Home Screen.');
      }
      return;
    }
    installPrompt = null;
    showInstall();
    try {
      await prompt.prompt();
    } catch {}
  });
showInstall();
// Chrome on Android wants a registered worker before it offers the install prompt; sw.js caches nothing. Needs a trusted HTTPS page.
if ('serviceWorker' in navigator && window.isSecureContext)
  navigator.serviceWorker.register('/sw.js').catch(() => {});
// The screen stays on while the stream is open (Screen Wake Lock: iOS 16.4+, Chrome).
let wakeLock = null;
async function keepAwake() {
  if (wakeLock || document.hidden || !navigator.wakeLock || el('player').hidden) return;
  try {
    const lock = await navigator.wakeLock.request('screen');
    wakeLock = lock;
    lock.addEventListener('release', () => {
      if (wakeLock === lock) wakeLock = null;
    });
  } catch {
    wakeLock = null;
  }
}
function releaseAwake() {
  const lock = wakeLock;
  wakeLock = null;
  try {
    lock?.release();
  } catch {}
}
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) void keepAwake();
});

// ---- interface: hide/show with a movable button, streamer mode, FPS overlay, menu ----
const prefs = {
  get(key, fallback) {
    try {
      const value = localStorage.getItem('nx.' + key);
      return value === null ? fallback : value;
    } catch {
      return fallback;
    }
  },
  set(key, value) {
    try {
      localStorage.setItem('nx.' + key, String(value));
    } catch {}
  },
};
const phoneLandscape = () => matchMedia('(max-height:520px) and (orientation:landscape)').matches,
  touchDevice = () => matchMedia('(pointer:coarse)').matches;
const ui = {
  hidden: false,
  streamer: prefs.get('streamer', '0') === '1',
  menu: prefs.get('menu', '1') === '1',
  pads: prefs.get('pads', '') === '' ? touchDevice() : prefs.get('pads', '0') === '1',
  rotate: prefs.get('rotate', '1') === '1',
};
let wasPlayer = false;
// A phone turned to landscape goes full screen by itself (the interface is hidden); see portraitMq for the way back.
const rotateMq = matchMedia('(pointer:coarse) and (max-height:520px) and (orientation:landscape)');
const inPlayer = () => !el('player').hidden;
function applyUi() {
  const body = document.body,
    player = inPlayer();
  if (!player) ui.hidden = false; // leaving the stream always brings the interface back
  // Each time the stream opens: phones in landscape start with the menu closed so the picture has the whole screen.
  if (player && !wasPlayer && phoneLandscape()) ui.menu = false;
  if (player && !wasPlayer && ui.rotate && rotateMq.matches) {
    ui.hidden = true;
    hintFullscreen();
  }
  wasPlayer = player;
  body.classList.toggle('in-player', player);
  body.classList.toggle('ui-hidden', player && ui.hidden);
  body.classList.toggle('streamer', ui.streamer);
  body.classList.toggle('menu-open', ui.menu);
  body.classList.toggle('show-pads', ui.pads);
  el('streamCircle').hidden = !(player && ui.hidden);
  if (el('streamCircle').hidden && !el('streamMenu').hidden) closeMenu();
  else if (!el('streamCircle').hidden) wakeCircle();
  // Streamer mode never shows what you type: the ID and password fields are masked and the field text is not kept.
  for (const id of ['sessionId', 'password'])
    el(id).type = ui.streamer ? 'password' : id === 'sessionId' ? 'text' : 'text';
  el('streamerJoin').setAttribute('aria-pressed', ui.streamer);
  el('streamerJoin').textContent = ui.streamer
    ? 'Streamer mode on: ID and password are hidden'
    : 'Streamer mode: hide ID and password';
  el('padsToggle').setAttribute('aria-pressed', ui.pads);
  el('padsToggle').querySelector('em').textContent = ui.pads ? 'On' : 'Off';
  el('rotateToggle').setAttribute('aria-pressed', ui.rotate);
  el('rotateToggle').querySelector('em').textContent = ui.rotate ? 'On' : 'Off';
  el('menu').setAttribute('aria-expanded', ui.menu);
  prefs.set('streamer', ui.streamer ? 1 : 0);
  if (!phoneLandscape()) prefs.set('menu', ui.menu ? 1 : 0);
  prefs.set('pads', ui.pads ? 1 : 0);
  prefs.set('rotate', ui.rotate ? 1 : 0);
  placeShowButton();
  if (player) void keepAwake();
  else releaseAwake();
}
new MutationObserver(applyUi).observe(el('player'), {
  attributes: true,
  attributeFilter: ['hidden'],
});
el('hideUi').addEventListener('click', () => {
  ui.hidden = true;
  applyUi();
});
el('streamerJoin').addEventListener('click', () => {
  ui.streamer = !ui.streamer;
  applyUi();
});
el('padsToggle').addEventListener('click', () => {
  ui.pads = !ui.pads;
  applyUi();
});
el('menu').addEventListener('click', () => {
  ui.menu = !ui.menu;
  applyUi();
});
el('rotateToggle').addEventListener('click', () => {
  ui.rotate = !ui.rotate;
  applyUi();
});
rotateMq.addEventListener('change', () => {
  fullscreenDeclined = false;
  if (!inPlayer() || !ui.rotate || !rotateMq.matches) return;
  if (!ui.hidden) {
    ui.hidden = true;
    applyUi();
  }
  // A tap a moment before turning the phone still counts, so full screen can start at once.
  if (wantsFullscreen() && navigator.userActivation?.isActive) requestPageFullscreen(false);
  else hintFullscreen();
});
// Turning a phone back to portrait always brings the interface back, however it was hidden; it can be hidden again afterwards.
const portraitMq = matchMedia('(pointer:coarse) and (orientation:portrait)');
portraitMq.addEventListener('change', () => {
  // The window shape can flip for a moment while a browser enters or leaves full screen; only a real portrait orientation counts.
  if (!inPlayer() || !portraitMq.matches || screen.orientation?.type?.startsWith('landscape')) return;
  fullscreenDeclined = false;
  el('fsHint').hidden = true;
  restoreUiOnExit = false;
  if (fullscreenElement()) exitFullscreen();
  if (!ui.hidden) return;
  ui.hidden = false;
  applyUi();
});
document.addEventListener('keydown', (event) => {
  if (
    (event.key === 'h' || event.key === 'H') &&
    !event.ctrlKey &&
    !event.metaKey &&
    !event.altKey &&
    inPlayer() &&
    !/^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement?.tagName || '')
  ) {
    event.preventDefault();
    ui.hidden = !ui.hidden;
    applyUi();
  }
});
// Stream menu: the hamburger circle (hidden-interface mode) is draggable anywhere (remembered), fades when idle, and a tap without
// movement opens the same menu as the Android app: Home, Controller >, Exit. "Show interface" is the web's extra row.
let showPos = null,
  menuSub = false,
  fadeTimer = 0;
try {
  showPos = JSON.parse(prefs.get('showPos', 'null'));
} catch {}
function placeShowButton() {
  const button = el('streamCircle');
  if (button.hidden) return;
  const margin = 10,
    w = button.offsetWidth || 52,
    x = showPos ? showPos.x : window.innerWidth - w - margin - 16,
    y = showPos ? showPos.y : ui.pads ? 76 : margin + 16; // below the shoulder buttons when the pads are shown
  button.style.left = Math.max(4, Math.min(window.innerWidth - w - 4, x)) + 'px';
  button.style.top = Math.max(4, Math.min(window.innerHeight - w - 4, y)) + 'px';
  if (!el('streamMenu').hidden) placeMenu();
}
function wakeCircle() {
  const button = el('streamCircle');
  button.classList.remove('idle');
  clearTimeout(fadeTimer);
  fadeTimer = setTimeout(() => {
    if (el('streamMenu').hidden) button.classList.add('idle');
  }, 3000);
}
function tapHome() {
  if (!grant || !padEnabled) {
    status(padEnabled ? 'Controls are not available yet.' : 'Turn the controller on to send HOME.');
    return;
  }
  pressed.set(-1, 'home');
  sendControls();
  setTimeout(() => {
    pressed.delete(-1);
    sendControls();
  }, 150);
}
// Back from the hamburger circle. A phone in landscape opens the side panel too: the picture shrinks to fit beside it.
function showInterface() {
  ui.hidden = false;
  if (phoneLandscape()) ui.menu = true;
  applyUi();
}
function menuRows() {
  const rows = [];
  if (!menuSub) {
    rows.push(['Home', tapHome, true]);
    rows.push(['Controller  ›', () => ((menuSub = true), openMenu()), false]);
    rows.push(['Show interface', showInterface, true]);
    rows.push(['Export logs', exportLogs, true]);
    rows.push(['Exit', () => el('disconnect').click(), true]);
  } else {
    rows.push(['‹  Back', () => ((menuSub = false), openMenu()), false]);
    rows.push([
      ui.pads ? 'On-screen buttons: On' : 'On-screen buttons: Off',
      () => {
        ui.pads = !ui.pads;
        applyUi();
      },
      true,
    ]);
    rows.push([touchEnabled ? 'Touch screen: On' : 'Touch screen: Off', () => el('touch').click(), true]);
    rows.push([
      'Configure controller',
      () => {
        showInterface();
        if (el('controller-configure').disabled) status('Press a controller button first.');
        else el('controller-configure').click();
      },
      true,
    ]);
  }
  return rows;
}
// The menu goes below the circle, else above it, else beside it (a phone in landscape is too short for either): never over the circle,
// because the click that follows the tap would then land on the menu row under the finger.
function placeMenu() {
  const circle = el('streamCircle'),
    menu = el('streamMenu'),
    gap = 8,
    w = menu.offsetWidth || 250,
    h = menu.offsetHeight,
    left = circle.offsetLeft,
    top = circle.offsetTop,
    size = circle.offsetWidth,
    fitX = (x) => Math.max(gap, Math.min(window.innerWidth - w - gap, x));
  let x = fitX(left + size / 2 - w / 2),
    y;
  if (top + size + gap + h <= window.innerHeight) y = top + size + gap;
  else if (top - gap - h >= gap) y = top - gap - h;
  else {
    x = left - gap - w >= gap ? left - gap - w : left + size + gap;
    y = Math.max(gap, Math.min(window.innerHeight - h - gap, top + size / 2 - h / 2));
  }
  menu.style.left = x + 'px';
  menu.style.top = y + 'px';
}
let menuOpenedAt = 0;
function openMenu() {
  const menu = el('streamMenu');
  menu.replaceChildren();
  menuOpenedAt = performance.now();
  for (const [label, action, closes] of menuRows()) {
    const row = document.createElement('button');
    row.type = 'button';
    row.setAttribute('role', 'menuitem');
    row.textContent = label;
    row.addEventListener('click', () => {
      // The click that ends the tap opening the menu arrives right after it: not a choice.
      if (performance.now() - menuOpenedAt < 350) return;
      if (closes) closeMenu();
      action();
    });
    menu.append(row);
  }
  menu.hidden = false;
  el('streamMenuCatcher').hidden = false;
  el('streamCircle').setAttribute('aria-expanded', 'true');
  placeMenu();
  wakeCircle();
}
function closeMenu() {
  menuSub = false;
  el('streamMenu').hidden = true;
  el('streamMenuCatcher').hidden = true;
  el('streamCircle').setAttribute('aria-expanded', 'false');
  wakeCircle();
}
el('streamMenuCatcher').addEventListener('pointerdown', (event) => {
  event.preventDefault();
  closeMenu();
});
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && !el('streamMenu').hidden) closeMenu();
});
(() => {
  const button = el('streamCircle');
  let start = null,
    moved = false;
  button.addEventListener('pointerdown', (event) => {
    start = { x: event.clientX, y: event.clientY, left: button.offsetLeft, top: button.offsetTop };
    moved = false;
    button.setPointerCapture(event.pointerId);
    wakeCircle();
  });
  button.addEventListener('pointermove', (event) => {
    if (!start) return;
    const dx = event.clientX - start.x,
      dy = event.clientY - start.y;
    if (!moved && Math.hypot(dx, dy) > 8) moved = true;
    if (moved) {
      showPos = { x: start.left + dx, y: start.top + dy };
      placeShowButton();
    }
  });
  const end = (event) => {
    if (!start) return;
    const tap = !moved && event.type === 'pointerup';
    if (moved) prefs.set('showPos', JSON.stringify(showPos));
    start = null;
    if (tap) {
      if (el('streamMenu').hidden) openMenu();
      else closeMenu();
    } else wakeCircle();
  };
  button.addEventListener('pointerup', end);
  button.addEventListener('pointercancel', end);
})();
window.addEventListener('resize', placeShowButton);
// Android long-presses open a context menu or select text; holding a pad button or the picture must not.
document.addEventListener('contextmenu', (event) => {
  if (event.target.closest?.('#controls, #streamCircle, #streamMenu, #viewport')) event.preventDefault();
});
// iOS Safari ignores user-scalable=no: block its pinch gestures and a second tap within 300 ms (double-tap zoom).
for (const type of ['gesturestart', 'gesturechange', 'gestureend'])
  document.addEventListener(type, (event) => event.preventDefault());
let lastTouchEnd = 0;
document.addEventListener(
  'touchend',
  (event) => {
    const now = Date.now();
    if (now - lastTouchEnd < 300) event.preventDefault();
    lastTouchEnd = now;
  },
  { passive: false }
);
applyUi();
// Opened from a session link: join at once, no password.
if (linkToken && el('sessionId').value)
  el('joinForm').dispatchEvent(new Event('submit', { cancelable: true }));
