// Claude Usage Monitor - Background Service Worker

importScripts('iot-push.js');

const KEEPALIVE_ALARM = 'keepalive';
const USAGE_POLL_ALARM = 'usage-poll';
const NOTIFIED_WINDOWS_KEY = 'notifiedWindows';

let usagePollInFlight = null;

setupAlarms();
restoreBadge();

// --- Keep service worker alive and own usage polling from the background ---
// setupAlarms() is called at script load (covers SW restart).
// onInstalled is only needed if install-specific behavior is added later.

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === KEEPALIVE_ALARM) {
    // Keepalive only prevents the service worker from sleeping.
    // Badge is restored on startup (line 12) and after each poll.
    return;
  }
  if (alarm.name === USAGE_POLL_ALARM) {
    pollUsage().catch(() => {});
  }
});

chrome.runtime.onMessage.addListener((msg) => {
  if (msg.type === 'SET_ORG_ID') {
    chrome.storage.local.get('settings', (data) => {
      const settings = { ...(data.settings || {}), autoDetect: false };
      chrome.storage.local.set({ orgId: msg.orgId, settings }, () => {
        pollUsage().catch(() => {});
      });
    });
    return;
  }

  if (msg.type === 'CLEAR_ORG_ID') {
    chrome.storage.local.remove('orgId');
    return;
  }

  if (msg.type === 'POLL_NOW') {
    pollUsage().catch(() => {});
  }
});

function setupAlarms() {
  chrome.alarms.create(KEEPALIVE_ALARM, { periodInMinutes: 1 });
  chrome.alarms.create(USAGE_POLL_ALARM, { periodInMinutes: 5 });
}

function restoreBadge() {
  chrome.storage.local.get(['currentUsage', 'settings'], (data) => {
    if (data.currentUsage) updateBadge(data.currentUsage, data.settings || {});
  });
}

async function pollUsage() {
  if (usagePollInFlight) return usagePollInFlight;

  usagePollInFlight = (async () => {
    const { orgId } = await chrome.storage.local.get('orgId');
    if (!orgId) return;

    const res = await fetch(`https://claude.ai/api/organizations/${orgId}/usage`, {
      credentials: 'include',
    });
    if (!res.ok) return;

    const data = await res.json();
    await handleUsageData(data, Date.now());
  })();

  try {
    await usagePollInFlight;
  } finally {
    usagePollInFlight = null;
  }
}

async function handleUsageData(raw, timestamp) {
  const stored = await chrome.storage.local.get([
    'history',
    'sessionStart',
    'settings',
    'lastTimestamp',
  ]);

  const snapshot = {
    timestamp,
    five_hour: raw.five_hour?.utilization ?? null,
    seven_day: raw.seven_day?.utilization ?? null,
    five_hour_resets_at: raw.five_hour?.resets_at ?? null,
    seven_day_resets_at: raw.seven_day?.resets_at ?? null,
  };

  const cutoff = Date.now() - 30 * 24 * 60 * 60 * 1000;
  const history = (stored.history || []).filter(h => h.timestamp > cutoff);
  history.push(snapshot);

  let sessionStart = stored.sessionStart;
  const lastTs = stored.lastTimestamp || 0;
  const isNewSession = (timestamp - lastTs) > 20 * 60 * 1000;
  if (!sessionStart || isNewSession) sessionStart = snapshot;

  await chrome.storage.local.set({
    history,
    currentUsage: snapshot,
    sessionStart,
    lastTimestamp: timestamp,
  });

  const settings = stored.settings || {};
  updateBadge(snapshot, settings);

  // Fire-and-forget: don't block notifications on LAN device response.
  if (settings.iotEnabled && settings.iotEndpoint) {
    const base = normalizeEndpoint(settings.iotEndpoint);
    pushToDevice(base, settings.iotApiKey || DEFAULT_DEVICE_API_KEY, buildDevicePayload(snapshot))
      .catch(() => {});
  }

  await checkPaceNotification(snapshot, settings);
}

function updateBadge(snapshot, settings) {
  const util = snapshot.five_hour ?? 0;
  const goal = settings?.goals?.five_hour ?? 80;

  let color = '#22c55e';
  if (util >= 90) color = '#ef4444';
  else if (util >= goal) color = '#f59e0b';

  const text = snapshot.five_hour !== null ? `${Math.round(util)}%` : '';
  chrome.action.setBadgeText({ text });
  chrome.action.setBadgeBackgroundColor({ color });
}

// Notify once per rate window, even across MV3 service worker restarts.
async function checkPaceNotification(snapshot, settings) {
  if (!settings.paceNotifications) return;

  const util = snapshot.five_hour ?? 0;
  const resetsAt = snapshot.five_hour_resets_at;
  if (!resetsAt) return;

  const now = Date.now();
  const remaining = new Date(resetsAt) - now;

  // Only touch storage when the notification condition could fire.
  if (util < 90 || remaining <= 60 * 60 * 1000) return;

  const stored = await chrome.storage.local.get(NOTIFIED_WINDOWS_KEY);
  const notifiedWindows = Object.fromEntries(
    Object.entries(stored[NOTIFIED_WINDOWS_KEY] || {})
      .filter(([resetTime]) => new Date(resetTime).getTime() > now)
  );

  if (!notifiedWindows[resetsAt]) {
    notifiedWindows[resetsAt] = now;
    await chrome.storage.local.set({ [NOTIFIED_WINDOWS_KEY]: notifiedWindows });
    chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon48.png',
      title: 'Claude Usage Warning',
      message: `5-hour window at ${util.toFixed(0)}% with ${Math.round(remaining / 3600000)}h remaining.`,
    });
  }
}
