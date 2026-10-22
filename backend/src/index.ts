import "dotenv/config";
import Fastify from "fastify";
import cors from "@fastify/cors";
import { MatomoPoller } from "./matomo.js";
import { BITMAP_SIZE_MAX, BITMAP_SIZE_MIN } from "./bitmap.js";
import { faviconRgb565 } from "./favicon.js";
import { flagRgb565, flagSvg } from "./flag.js";

const PORT = Number(process.env.PORT ?? 3001);
const POLL_INTERVAL_MS = Number(process.env.POLL_INTERVAL_MS ?? 30_000);

const SIZE_ERROR = `sizes must be integers ${BITMAP_SIZE_MIN}-${BITMAP_SIZE_MAX}`;
function validSize(n: number): boolean {
  return Number.isInteger(n) && n >= BITMAP_SIZE_MIN && n <= BITMAP_SIZE_MAX;
}

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

// New or changed visits since `after` (oldest first), optionally only the
// `limit` most recent; clients only keep the last few
app.get<{ Querystring: { after?: string; limit?: string } }>("/api/visits", async (req) => {
  const after = Number(req.query.after ?? 0);
  const limit = Number(req.query.limit ?? 0);
  return {
    visits: poller.visitsSince(
      Number.isNaN(after) ? 0 : after,
      Number.isInteger(limit) && limit > 0 ? limit : undefined,
    ),
    cursor: poller.lastSeq,
  };
});

// Favicon as a raw RGB565 bitmap for the CYD: size*size little-endian uint16
// pixels on a white background. Only hosts seen in visits are served.
app.get<{ Querystring: { host?: string; size?: string } }>("/api/favicon.rgb565", async (req, reply) => {
  const host = (req.query.host ?? "").toLowerCase();
  const size = Number(req.query.size ?? 32);
  if (!validSize(size)) return reply.code(400).send({ error: SIZE_ERROR });
  if (!poller.hasHost(host)) return reply.code(404).send({ error: "unknown host" });

  const bitmap = await faviconRgb565(host, size);
  if (!bitmap) return reply.code(404).send({ error: "no favicon" });
  return reply
    .header("content-type", "application/octet-stream")
    .header("cache-control", "public, max-age=86400")
    .send(bitmap);
});

// Country flag as a raw RGB565 bitmap for the CYD (same format as favicons),
// for the lower-case ISO code in a visit's countryCode
app.get<{ Querystring: { cc?: string; w?: string; h?: string } }>("/api/flag.rgb565", async (req, reply) => {
  const width = Number(req.query.w ?? 18);
  const height = Number(req.query.h ?? 12);
  if (!validSize(width) || !validSize(height)) return reply.code(400).send({ error: SIZE_ERROR });

  const bitmap = flagRgb565((req.query.cc ?? "").toLowerCase(), width, height);
  if (!bitmap) return reply.code(404).send({ error: "no flag" });
  return reply
    .header("content-type", "application/octet-stream")
    .header("cache-control", "public, max-age=604800")
    .send(await bitmap);
});

// The same flag as an SVG, for the web card (emoji flags don't render on
// Windows, and this keeps web and CYD identical)
app.get<{ Querystring: { cc?: string } }>("/api/flag.svg", async (req, reply) => {
  const svg = flagSvg((req.query.cc ?? "").toLowerCase());
  if (!svg) return reply.code(404).send({ error: "no flag" });
  return reply
    .header("content-type", "image/svg+xml")
    .header("cache-control", "public, max-age=604800")
    .send(svg);
});

app.get("/api/health", async () => ({ ok: true, sites: poller.siteIds.length }));

await app.listen({ port: PORT, host: "0.0.0.0" });
