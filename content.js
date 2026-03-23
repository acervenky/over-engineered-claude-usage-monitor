// Claude Usage Monitor - Content Script (isolated world)
// Only detects the org ID. Background owns polling.
// Fetch interception happens via injected.js in the main world.

let orgId = null;

function setOrgId(id) {
  if (orgId === id) return;
  orgId = id;
  chrome.runtime.sendMessage({ type: 'SET_ORG_ID', orgId });
}

// --- Listen for org ID posted from injected main-world script ---
window.addEventListener('message', (e) => {
  if (e.source !== window) return;
  if (e.data?.type === 'CLAUDE_USAGE_ORG_ID' && e.data.orgId
      && /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(e.data.orgId)) {
    setOrgId(e.data.orgId);
  }
});

// --- Inject main-world script (only if auto-detect enabled) ---
function injectInterceptor() {
  const s = document.createElement('script');
  s.src = chrome.runtime.getURL('injected.js');
  s.onload = () => s.remove();
  (document.head || document.documentElement).appendChild(s);
}

// --- Init ---
async function init() {
  const stored = await chrome.storage.local.get(['orgId', 'settings']);
  const settings = stored.settings || {};

  if (stored.orgId) {
    orgId = stored.orgId;
    return;
  }

  if (settings.autoDetect) {
    injectInterceptor();
  }
}

init();

chrome.runtime.onMessage.addListener((msg) => {
  if (msg.type === 'ORG_ID_UPDATED') {
    orgId = msg.orgId;
  }
  if (msg.type === 'AUTO_DETECT_ENABLED' && !orgId) injectInterceptor();
});
