// Shared usage state — updated by HTTP server, read by actions

export interface UsageSnapshot {
  fiveHour: number | null;
  sevenDay: number | null;
  fiveHourResetsAt: string | null;
  sevenDayResetsAt: string | null;
  timestamp: number;
}

export const store = {
  usage: null as UsageSnapshot | null,
  fiveHourActions: new Set<{ setTitle: (t: string) => Promise<void> }>(),
  sevenDayActions: new Set<{ setTitle: (t: string) => Promise<void> }>(),
};

// Format seconds into countdown string
export function formatCountdown(resetsAt: string | null): string {
  if (!resetsAt) return "—";
  const diff = new Date(resetsAt).getTime() - Date.now();
  if (diff <= 0) return "resetting";
  const h = Math.floor(diff / 3600000);
  const m = Math.floor((diff % 3600000) / 60000);
  if (h > 24) {
    const d = Math.floor(h / 24);
    return `${d}d ${h % 24}h`;
  }
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}

// Build button title — two lines: utilization % + countdown
export function buildTitle(util: number | null, resetsAt: string | null): string {
  if (util === null) return "—\nno data";
  const pct = `${util.toFixed(1)}%`;
  const cd = formatCountdown(resetsAt);
  return `${pct}\n${cd}`;
}

// Push latest data to all registered action instances
export async function pushToActions(): Promise<void> {
  if (!store.usage) return;

  const t5 = buildTitle(store.usage.fiveHour, store.usage.fiveHourResetsAt);
  const t7 = buildTitle(store.usage.sevenDay, store.usage.sevenDayResetsAt);

  const updates: Promise<void>[] = [];
  for (const action of store.fiveHourActions) updates.push(action.setTitle(t5));
  for (const action of store.sevenDayActions)  updates.push(action.setTitle(t7));
  await Promise.allSettled(updates);
}
