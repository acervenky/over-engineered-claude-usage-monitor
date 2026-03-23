// Claude Usage Monitor - Popup Script

const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

document.addEventListener('DOMContentLoaded', async () => {

  // ── Header buttons — wire up unconditionally before any early returns ──
  document.getElementById('settings-btn').addEventListener('click', () => {
    chrome.tabs.create({ url: chrome.runtime.getURL('options.html') });
  });

  document.getElementById('refresh-btn').addEventListener('click', async () => {
    chrome.runtime.sendMessage({ type: 'POLL_NOW' }).catch(() => {});
    setTimeout(() => window.location.reload(), 2000);
  });

  const data = await chrome.storage.local.get([
    'currentUsage', 'sessionStart', 'history', 'settings', 'orgId'
  ]);

  const usage   = data.currentUsage;
  const session = data.sessionStart;
  const history = data.history || [];
  const settings = data.settings || {};
  const goals = settings.goals || { five_hour: 80, seven_day: 70 };

  // Show/hide no-data state
  if (!usage || usage.five_hour === null) {
    document.getElementById('no-data').classList.remove('hidden');
    document.getElementById('main-content').classList.add('hidden');
    return;
  }

  // Last updated
  if (usage.timestamp) {
    const mins = Math.round((Date.now() - usage.timestamp) / 60000);
    const txt = mins < 1 ? 'just now' : `${mins}m ago`;
    document.getElementById('last-updated').textContent = txt;
  }

  // ── Bars ──
  renderBar({
    fillId: '5h-fill',
    goalId: '5h-goal',
    utilId: '5h-util',
    countdownId: '5h-countdown',
    sessionId: '5h-session',
    paceId: '5h-pace',
    util: usage.five_hour,
    resetsAt: usage.five_hour_resets_at,
    goal: goals.five_hour,
    sessionUtil: session?.five_hour,
  });

  renderBar({
    fillId: '7d-fill',
    goalId: '7d-goal',
    utilId: '7d-util',
    countdownId: '7d-countdown',
    sessionId: null,
    paceId: null,
    util: usage.seven_day,
    resetsAt: usage.seven_day_resets_at,
    goal: goals.seven_day,
    sessionUtil: null,
  });

  // ── Charts ──
  drawHeatmap(history);
  drawWeekly(history);

  // ── Export CSV ──
  document.getElementById('export-btn').addEventListener('click', () => exportCSV(history));
});

// ── BAR RENDERER ──
function renderBar({ fillId, goalId, utilId, countdownId, sessionId, paceId, util, resetsAt, goal, sessionUtil }) {
  const pct = util ?? 0;
  const fill = document.getElementById(fillId);
  const goalEl = document.getElementById(goalId);
  const utilEl = document.getElementById(utilId);
  const countdown = document.getElementById(countdownId);
  const sessionEl = sessionId ? document.getElementById(sessionId) : null;
  const paceEl = paceId ? document.getElementById(paceId) : null;

  // Bar fill
  fill.style.width = `${Math.min(pct, 100)}%`;
  fill.className = 'bar-fill' + (pct >= 90 ? ' crit' : pct >= goal ? ' warn' : '');

  // Util %
  utilEl.textContent = `${pct.toFixed(1)}%`;
  if (pct >= 90) utilEl.style.color = 'var(--red)';
  else if (pct >= goal) utilEl.style.color = 'var(--amber)';
  else utilEl.style.color = 'var(--text)';

  // Goal marker
  if (goalEl) goalEl.style.left = `${goal}%`;

  // Countdown
  if (countdown && resetsAt) {
    const diff = new Date(resetsAt) - Date.now();
    if (diff > 0) {
      const h = Math.floor(diff / 3600000);
      const m = Math.floor((diff % 3600000) / 60000);
      countdown.textContent = `resets ${h}h ${m}m`;
    }
  }

  // Session delta
  if (sessionEl && sessionUtil !== null && sessionUtil !== undefined) {
    const delta = pct - sessionUtil;
    sessionEl.textContent = delta >= 0 ? `+${delta.toFixed(1)}% this session` : '';
  }

  // Pace badge
  if (paceEl && resetsAt) {
    const diff = new Date(resetsAt) - Date.now();
    const hoursLeft = diff / 3600000;
    if (pct >= 90 && hoursLeft > 1) {
      paceEl.textContent = 'CRITICAL';
      paceEl.className = 'pace-badge pace-crit';
    } else if (pct >= goal) {
      paceEl.textContent = 'WARN';
      paceEl.className = 'pace-badge pace-warn';
    } else {
      paceEl.textContent = 'OK';
      paceEl.className = 'pace-badge pace-ok';
    }
  }
}

// ── HEATMAP (24 cols × 1 row of colored cells) ──
// Groups history by hour-of-day, shows average 5hr utilization
function drawHeatmap(history) {
  const canvas = document.getElementById('heatmap');
  const ctx = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0, 0, W, H);

  // Only last 14 days
  const cutoff = Date.now() - 14 * 24 * 60 * 60 * 1000;
  const recent = history.filter(h => h.timestamp > cutoff && h.five_hour !== null);

  // Bucket by hour-of-day (0–23)
  const buckets = Array.from({ length: 24 }, () => []);
  for (const h of recent) {
    const hour = new Date(h.timestamp).getHours();
    buckets[hour].push(h.five_hour);
  }

  const cellW = W / 24;
  const cellH = H;
  const gap = 2;

  for (let i = 0; i < 24; i++) {
    const vals = buckets[i];
    const avg = vals.length ? vals.reduce((a, b) => a + b, 0) / vals.length : -1;

    let fill;
    if (avg < 0) {
      fill = '#1a1a20'; // no data
    } else {
      fill = utilColor(avg);
    }

    const x = i * cellW + gap / 2;
    ctx.fillStyle = fill;
    ctx.beginPath();
    roundRect(ctx, x, gap / 2, cellW - gap, cellH - gap, 2);
    ctx.fill();

    // Util text if high enough
    if (avg >= 20) {
      ctx.fillStyle = 'rgba(0,0,0,0.6)';
      ctx.font = `bold 7px 'JetBrains Mono', monospace`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(`${Math.round(avg)}`, x + (cellW - gap) / 2, cellH / 2);
    }
  }
}

// ── WEEKLY BARS (Mon–Sun, 4wk avg) ──
function drawWeekly(history) {
  const canvas = document.getElementById('weekly');
  const ctx = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height - 4;
  ctx.clearRect(0, 0, W, H + 4);

  const cutoff = Date.now() - 28 * 24 * 60 * 60 * 1000;
  const recent = history.filter(h => h.timestamp > cutoff && h.five_hour !== null);

  // Bucket by day-of-week (0=Sun)
  const buckets = Array.from({ length: 7 }, () => []);
  for (const h of recent) {
    const dow = new Date(h.timestamp).getDay();
    buckets[dow].push(h.five_hour);
  }

  const barW = W / 7;
  const gap = 3;

  // Build axis labels
  const axisEl = document.getElementById('weekly-axis');
  axisEl.innerHTML = '';
  DAYS.forEach(d => {
    const span = document.createElement('span');
    span.textContent = d;
    axisEl.appendChild(span);
  });

  for (let i = 0; i < 7; i++) {
    const vals = buckets[i];
    const avg = vals.length ? vals.reduce((a, b) => a + b, 0) / vals.length : 0;

    const x = i * barW + gap / 2;
    const barH = Math.max(2, (avg / 100) * H);

    // Background track
    ctx.fillStyle = '#1a1a20';
    ctx.beginPath();
    roundRect(ctx, x, 0, barW - gap, H, 3);
    ctx.fill();

    // Bar
    ctx.fillStyle = avg > 0 ? utilColor(avg) : '#1a1a20';
    ctx.beginPath();
    roundRect(ctx, x, H - barH, barW - gap, barH, 3);
    ctx.fill();

    // Value label on bar
    if (avg >= 5) {
      ctx.fillStyle = 'rgba(0,0,0,0.55)';
      ctx.font = `bold 7px 'JetBrains Mono', monospace`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(`${Math.round(avg)}`, x + (barW - gap) / 2, H - barH / 2);
    }
  }
}

// ── COLOR SCALE ──
function utilColor(pct) {
  if (pct >= 90) return '#ef4444';
  if (pct >= 70) return '#f59e0b';
  if (pct >= 40) return '#a78bfa';
  if (pct >= 10) return '#6366f1';
  return '#3b3b5a';
}

// ── ROUNDED RECT HELPER ──
function roundRect(ctx, x, y, w, h, r) {
  ctx.roundRect
    ? ctx.roundRect(x, y, w, h, r)
    : (ctx.rect(x, y, w, h)); // fallback
}

// ── EXPORT CSV ──
function exportCSV(history) {
  const rows = ['timestamp,five_hour_util,seven_day_util,five_hour_resets_at,seven_day_resets_at'];
  for (const h of history) {
    rows.push([
      new Date(h.timestamp).toISOString(),
      h.five_hour ?? '',
      h.seven_day ?? '',
      h.five_hour_resets_at ?? '',
      h.seven_day_resets_at ?? '',
    ].join(','));
  }
  const blob = new Blob([rows.join('\n')], { type: 'text/csv' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `claude-usage-${new Date().toISOString().slice(0, 10)}.csv`;
  a.click();
  URL.revokeObjectURL(url);
}
