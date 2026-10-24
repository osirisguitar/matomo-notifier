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
  /** Title of the latest page viewed in the visit, when Matomo has one */
  lastPageTitle?: string;
}

interface RawVisit {
  idVisit: string;
  siteName?: string;
  countryCode?: string;
  country?: string;
  city?: string;
  deviceType?: string;
  actions?: number;
  actionDetails?: Array<{ type: string; url?: string; pageTitle?: string }>;
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

// Matomo HTML-encodes some characters in titles
function decodeEntities(s: string): string {
  return s
    .replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(Number(n)))
    .replace(/&(quot|apos|lt|gt|amp);/g, (_, e) =>
      ({ quot: '"', apos: "'", lt: "<", gt: ">", amp: "&" })[e as string]!,
    );
}

function lastPageTitle(raw: RawVisit): string | undefined {
  const title = raw.actionDetails?.findLast((a) => a.type === "action" && a.pageTitle)?.pageTitle;
  return title ? decodeEntities(title).trim() || undefined : undefined;
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
    lastPageTitle: lastPageTitle(raw),
  };
}
