export interface VisitNotification {
  seq: number;
  id: string;
  siteId: number;
  siteName: string;
  host: string | undefined;
  actions: number;
  goals: number;
  countryCode: string;
  country?: string;
  city?: string;
  deviceType: "mobile" | "desktop";
  visitStart: string;
}

interface RawVisit {
  idVisit: string;
  siteName?: string;
  countryCode?: string;
  country?: string;
  city?: string;
  deviceType?: string;
  actions?: number;
  actionDetails?: Array<{ type: string; url?: string }>;
  firstActionTimestamp?: number;
}

const MOBILE_DEVICE_TYPES = new Set([
  "smartphone",
  "tablet",
  "phablet",
  "tv",
  "console",
]);

function siteHost(raw: RawVisit): string | undefined {
  const firstUrl = raw.actionDetails?.find((a) => a.url)?.url;
  if (!firstUrl) return undefined;
  try {
    return new URL(firstUrl).hostname;
  } catch {
    return undefined;
  }
}

export function mapVisit(
  raw: RawVisit,
  seq: number,
  siteId: number,
  fallbackSiteName: string,
): VisitNotification {
  const actionDetails = raw.actionDetails ?? [];
  const goals = actionDetails.filter((a) => a.type === "goal").length;
  const actions =
    raw.actions ?? actionDetails.filter((a) => a.type !== "goal").length;

  const device = (raw.deviceType ?? "unknown").toLowerCase();

  return {
    seq,
    id: `${siteId}-${raw.idVisit}`,
    siteId,
    siteName: raw.siteName || fallbackSiteName,
    host: siteHost(raw),
    actions,
    goals,
    countryCode: (raw.countryCode ?? "").toLowerCase(),
    country: raw.country || undefined,
    city: raw.city || undefined,
    deviceType: MOBILE_DEVICE_TYPES.has(device) ? "mobile" : "desktop",
    visitStart: raw.firstActionTimestamp
      ? new Date(raw.firstActionTimestamp * 1000).toISOString()
      : new Date().toISOString(),
  };
}
