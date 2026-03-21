// Claude Usage Monitor - Background Service Worker

// --- Restore badge on service worker startup (after idle unload) ---
chrome.storage.local.get(['currentUsage', 'settings'], (data) => {
  if (data.currentUsage) updateBadge(data.currentUsage, data.settings || {});
});

// --- Keep service worker alive with a periodic alarm ---
chrome.runtime.onInstalled.addListener(() => {
  chrome.alarms.create('keepalive', { periodInMinutes: 0.4 });
});
chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === 'keepalive') {
    // Re-restore badge each time in case it was cleared
    chrome.storage.local.get(['currentUsage', 'settings'], (data) => {
      if (data.currentUsage) updateBadge(data.currentUsage, data.settings || {});
    });
  }
});

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg.type === 'SET_ORG_ID') {
    chrome.storage.local.get('settings', (data) => {
      const settings = { ...(data.settings || {}), autoDetect: false };
      chrome.storage.local.set({ orgId: msg.orgId, settings });
    });
  }
  if (msg.type === 'USAGE_DATA') {
    handleUsageData(msg.data, msg.timestamp);
  }
});

async function handleUsageData(raw, timestamp) {
  const stored = await chrome.storage.local.get(['history', 'sessionStart', 'settings', 'lastTimestamp', 'orgId']);

  const snapshot = {
    timestamp,
    five_hour: raw.five_hour?.utilization ?? null,
    seven_day: raw.seven_day?.utilization ?? null,
    five_hour_resets_at: raw.five_hour?.resets_at ?? null,
    seven_day_resets_at: raw.seven_day?.resets_at ?? null,
  };

  // --- History: keep 30 days ---
  const cutoff = Date.now() - 30 * 24 * 60 * 60 * 1000;
  const history = (stored.history || []).filter(h => h.timestamp > cutoff);
  history.push(snapshot);

  // --- Session start snapshot ---
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

  // --- Badge ---
  const settings = stored.settings || {};
  updateBadge(snapshot, settings);

  // --- Stream Deck push (localhost:57891/usage) ---
  fetch('http://127.0.0.1:57891/usage', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      fiveHour:         snapshot.five_hour,
      sevenDay:         snapshot.seven_day,
      fiveHourResetsAt: snapshot.five_hour_resets_at,
      sevenDayResetsAt: snapshot.seven_day_resets_at,
      timestamp:        snapshot.timestamp,
    }),
  }).catch(() => {}); // silent — plugin may not be running
  if (settings.iotEnabled && settings.iotEndpoint && stored.orgId) {
    chrome.cookies.get({ url: 'https://claude.ai', name: 'sessionKey' }, (cookie) => {
      if (!cookie) return;
      const sessionKey = `sessionKey=${cookie.value}`;
      const base = settings.iotEndpoint.startsWith('http')
        ? settings.iotEndpoint : `http://${settings.iotEndpoint}`;
      fetch(`${base}/configure`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ orgId: stored.orgId, sessionKey }),
      }).catch(() => {});
    });
  }

  // --- Pace notifications ---
  checkPaceNotification(snapshot, settings, stored.history || []);
}

function updateBadge(snapshot, settings) {
  const util = snapshot.five_hour ?? 0;
  const goal = settings?.goals?.five_hour ?? 80;

  let color = '#22c55e';
  if (util >= 90) color = '#ef4444';
  else if (util >= goal) color = '#f59e0b';

  // Show badge even at 0% so it's always visible once data exists
  const text = snapshot.five_hour !== null ? `${Math.round(util)}%` : '';
  chrome.action.setBadgeText({ text });
  chrome.action.setBadgeBackgroundColor({ color });
}

// Notify once per window if pace is critical (90%+ with >1hr remaining)
const notifiedWindows = new Set();

async function checkPaceNotification(snapshot, settings, history) {
  if (!settings.paceNotifications) return;
  const util = snapshot.five_hour ?? 0;
  const resetsAt = snapshot.five_hour_resets_at;
  if (!resetsAt) return;

  const remaining = new Date(resetsAt) - Date.now();
  const windowKey = resetsAt;

  if (util >= 90 && remaining > 60 * 60 * 1000 && !notifiedWindows.has(windowKey)) {
    notifiedWindows.add(windowKey);
    chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon48.png',
      title: 'Claude Usage Warning',
      message: `5-hour window at ${util.toFixed(0)}% with ${Math.round(remaining / 3600000)}h remaining.`,
    });
  }
}

