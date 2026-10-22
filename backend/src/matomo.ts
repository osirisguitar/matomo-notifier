import { mapVisit, type VisitNotification } from "./visit.js";

interface PollerOptions {
  matomoUrl: string;
  apiToken: string;
  siteIds: string[];
  intervalMs: number;
}

const BUFFER_MAX = 200;
const SEEN_MAX = 500;
const SEEN_KEEP = 200;

interface SiteState {
  lastVisitId: number;
  seenIds: Set<number>;
  baselineSet: boolean;
}

interface RawVisit {
  idVisit: string;
  siteName?: string;
  countryCode?: string;
  country?: string;
  city?: string;
  deviceType?: string;
  actions?: number;
  actionDetails?: Array<{ type: string }>;
  firstActionTimestamp?: number;
}

export class MatomoPoller {
  readonly siteIds: string[];
  cursor = 0;
  private buffer: Array<VisitNotification & { seq: number }> = [];
  private siteStates = new Map<string, SiteState>();
  private opts: PollerOptions;
  private timer?: NodeJS.Timeout;
  private seq = 0;

  constructor(opts: PollerOptions) {
    this.opts = opts;
    this.siteIds = opts.siteIds;
  }

  start() {
    if (this.timer) return;
    void this.pollOnce();
    this.timer = setInterval(() => void this.pollOnce(), this.opts.intervalMs);
  }

  stop() {
    if (this.timer) clearInterval(this.timer);
    this.timer = undefined;
  }

  visitsSince(after: number): VisitNotification[] {
    return this.buffer.filter((v) => v.seq > after);
  }

  /** Whether any buffered visit came from this host (favicon allowlist). */
  hasHost(host: string): boolean {
    return this.buffer.some((v) => v.host === host);
  }

  get lastSeq(): number {
    return this.seq;
  }

  private async pollOnce() {
    await Promise.all(this.siteIds.map((id) => this.pollSite(id)));
  }

  private async pollSite(siteId: string) {
    let state = this.siteStates.get(siteId);
    if (!state) {
      state = { lastVisitId: 0, seenIds: new Set(), baselineSet: false };
      this.siteStates.set(siteId, state);
    }

    let visits: RawVisit[];
    try {
      const url = new URL("/index.php", this.opts.matomoUrl);
      url.searchParams.set("module", "API");
      url.searchParams.set("method", "Live.getLastVisitsDetails");
      url.searchParams.set("format", "JSON");
      url.searchParams.set("idSite", siteId);
      url.searchParams.set("period", "day");
      url.searchParams.set("date", "last2");
      url.searchParams.set("filter_limit", "100");

      // Some Matomo instances require the token to be sent via POST.
      const res = await fetch(url, {
        method: "POST",
        body: new URLSearchParams({ token_auth: this.opts.apiToken }),
        signal: AbortSignal.timeout(15_000),
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const body: unknown = await res.json();
      if (!Array.isArray(body)) {
        throw new Error(`Unexpected API response: ${JSON.stringify(body)}`);
      }
      visits = body as RawVisit[];
    } catch (err) {
      console.error(`[matomo] site ${siteId} poll failed:`, err);
      return;
    }

    let fresh = 0;
    for (const raw of visits) {
      const idNum = Number(raw.idVisit);
      if (!Number.isFinite(idNum)) continue;
      if (state.seenIds.has(idNum)) continue;
      state.seenIds.add(idNum);

      if (!state.baselineSet) {
        // First poll for this site: record baseline without notifying.
        state.lastVisitId = Math.max(state.lastVisitId, idNum);
        continue;
      }

      if (idNum <= state.lastVisitId) continue;
      state.lastVisitId = Math.max(state.lastVisitId, idNum);

      const mapped = mapVisit(raw, ++this.seq, Number(siteId), `Site ${siteId}`);
      this.buffer.push(mapped);
      fresh++;
    }

    // Once a baseline batch has been processed, later polls emit new visits.
    state.baselineSet = true;

    if (state.seenIds.size > SEEN_MAX) {
      state.seenIds = new Set([...state.seenIds].sort((a, b) => b - a).slice(0, SEEN_KEEP));
    }
    if (this.buffer.length > BUFFER_MAX) {
      this.buffer = this.buffer.slice(-BUFFER_MAX);
    }

    if (fresh > 0) console.log(`[matomo] site ${siteId}: ${fresh} new visit(s)`);
  }
}
