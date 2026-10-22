import "dotenv/config";
import Fastify from "fastify";
import cors from "@fastify/cors";
import { MatomoPoller } from "./matomo.js";

const PORT = Number(process.env.PORT ?? 3001);
const POLL_INTERVAL_MS = Number(process.env.POLL_INTERVAL_MS ?? 30_000);

const app = Fastify({ logger: true });
await app.register(cors, { origin: true });

const poller = new MatomoPoller({
  matomoUrl: process.env.MATOMO_URL ?? "",
  apiToken: process.env.MATOMO_API_TOKEN ?? "",
  siteIds: (process.env.MATOMO_SITE_IDS ?? "")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean),
  intervalMs: POLL_INTERVAL_MS,
});
poller.start();

app.get<{ Querystring: { after?: string } }>("/api/visits", async (req) => {
  const after = Number(req.query.after ?? 0);
  return {
    visits: poller.visitsSince(Number.isNaN(after) ? 0 : after),
    cursor: poller.lastSeq,
  };
});

app.get("/api/health", async () => ({ ok: true, sites: poller.siteIds.length }));

await app.listen({ port: PORT, host: "0.0.0.0" });
