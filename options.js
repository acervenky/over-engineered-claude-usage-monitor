// Claude Usage Monitor - Options Script
// iot-push.js is loaded via <script> tag before this file

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

const _flashTimers = new WeakMap();
function flashStatus(el, color, message, ms = 3000) {
  clearTimeout(_flashTimers.get(el));
  el.style.color = color;
  el.textContent = message;
  _flashTimers.set(el, setTimeout(() => el.textContent = '', ms));
}

async function load() {
  const data = await chrome.storage.local.get(['orgId', 'settings']);
  const s = data.settings || {};
  const goals = s.goals || {};

  document.getElementById('org-id').value        = data.orgId || '';
  document.getElementById('auto-detect').checked = s.autoDetect === true;
  document.getElementById('goal-5h').value       = goals.five_hour ?? 80;
  document.getElementById('goal-7d').value       = goals.seven_day ?? 70;
  document.getElementById('pace-notifs').checked = !!s.paceNotifications;
  document.getElementById('iot-endpoint').value  = s.iotEndpoint || '';
  document.getElementById('iot-api-key').value   = s.iotApiKey || DEFAULT_DEVICE_API_KEY;
  document.getElementById('iot-enabled').checked = !!s.iotEnabled;

  syncAutoDetect();
}

function syncAutoDetect() {
  const on = document.getElementById('auto-detect').checked;
  const orgInput = document.getElementById('org-id');
  orgInput.disabled = on;
  orgInput.style.opacity = on ? '0.4' : '1';
}

document.getElementById('auto-detect').addEventListener('change', syncAutoDetect);

document.getElementById('push-btn').addEventListener('click', async () => {
  const endpoint = document.getElementById('iot-endpoint').value.trim();
  const apiKey   = document.getElementById('iot-api-key').value.trim();
  const status   = document.getElementById('push-status');

  if (!endpoint) {
    flashStatus(status, 'var(--red)', '\u2717 enter device IP first');
    return;
  }
  if (!apiKey) {
    flashStatus(status, 'var(--red)', '\u2717 enter device API key');
    return;
  }

  const stored = await chrome.storage.local.get('currentUsage');
  if (!stored.currentUsage) {
    flashStatus(status, 'var(--red)', '\u2717 no usage data yet \u2014 open claude.ai first');
    return;
  }

  const btn = document.getElementById('push-btn');
  btn.disabled = true;
  status.style.color = 'var(--dim)';
  status.textContent = 'pushing...';

  try {
    const base = normalizeEndpoint(endpoint);
    const res = await pushToDevice(base, apiKey, buildDevicePayload(stored.currentUsage));
    if (res.ok) {
      flashStatus(status, 'var(--green)', '\u2713 pushed to device', 4000);
    } else if (res.status === 401) {
      flashStatus(status, 'var(--red)', '\u2717 API key rejected by device', 4000);
    } else {
      flashStatus(status, 'var(--red)', `\u2717 device returned ${res.status}`, 4000);
    }
  } catch (e) {
    flashStatus(status, 'var(--red)', '\u2717 could not reach device', 4000);
  }

  btn.disabled = false;
});

document.getElementById('save-btn').addEventListener('click', async () => {
  const orgId      = document.getElementById('org-id').value.trim();
  const autoDetect = document.getElementById('auto-detect').checked;

  if (!autoDetect && orgId && !UUID_RE.test(orgId)) {
    flashStatus(document.getElementById('save-status'), 'var(--red)', '\u2717 invalid UUID format');
    return;
  }

  const settings = {
    autoDetect,
    goals: {
      five_hour: parseInt(document.getElementById('goal-5h').value, 10) || 80,
      seven_day: parseInt(document.getElementById('goal-7d').value, 10) || 70,
    },
    paceNotifications: document.getElementById('pace-notifs').checked,
    iotEndpoint:       document.getElementById('iot-endpoint').value.trim(),
    iotApiKey:         document.getElementById('iot-api-key').value.trim(),
    iotEnabled:        document.getElementById('iot-enabled').checked,
  };

  const toSave = { settings };
  if (!autoDetect && orgId) toSave.orgId = orgId;
  await chrome.storage.local.set(toSave);
  if (autoDetect) await chrome.storage.local.remove('orgId');
  if (!autoDetect && orgId) chrome.runtime.sendMessage({ type: 'SET_ORG_ID', orgId }).catch(() => {});
  if (autoDetect) chrome.runtime.sendMessage({ type: 'CLEAR_ORG_ID' }).catch(() => {});

  const tabs = await chrome.tabs.query({ url: 'https://claude.ai/*' });
  if (autoDetect) {
    for (const tab of tabs) chrome.tabs.reload(tab.id);
  } else if (orgId) {
    for (const tab of tabs)
      chrome.tabs.sendMessage(tab.id, { type: 'ORG_ID_UPDATED', orgId }).catch(() => {});
  }

  const st = document.getElementById('save-status');
  flashStatus(st, 'var(--green)', autoDetect ? '\u2713 saved \u2014 reloading claude.ai\u2026' : '\u2713 saved');
});

document.getElementById('clear-history').addEventListener('click', async () => {
  if (!confirm('Clear all usage history? This cannot be undone.')) return;
  await chrome.storage.local.remove(['history', 'currentUsage', 'sessionStart', 'lastTimestamp']);
  alert('History cleared.');
});

load();
