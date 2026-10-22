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
