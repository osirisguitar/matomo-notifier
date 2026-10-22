# Matomo Notifier

Watch one or more Matomo sites and get a popup card in the browser every time a
new visit is detected.

## Architecture

- `backend/` — Fastify (TypeScript). Polls the Matomo Live API for each site
  listed in `.env`, keeps the last 100 detected visits in memory and exposes a
  small HTTP API.
- `frontend/` — Vite + vanilla TypeScript. Polls the backend every 2 s and shows
  a notification card for each new visit. Keeps the last 10 in memory; click the
  card to step back through previous notifications.

## Setup

Create `backend/.env` (see `backend/.env.example`):

```
MATOMO_URL=https://your-matomo.example
MATOMO_API_TOKEN=your-token
MATOMO_SITE_IDS=1,2,3
POLL_INTERVAL_MS=30000
PORT=3001
```

The token needs at least *view* access and is sent to Matomo via POST.

## Run

```sh
cd backend  && npm install && npm run dev   # http://localhost:3001
cd frontend && npm install && npm run dev   # http://localhost:5173
```

The frontend dev server proxies `/api` to the backend.

## API

- `GET /api/health` — `{ ok, sites }`
- `GET /api/visits?after=<seq>` — new visits since `seq`, plus the current
  `cursor` to pass in the next request.
- `GET /api/favicon.rgb565?host=<host>&size=<4-64>` — the site's favicon as a
  raw bitmap for the CYD display: `size*size` little-endian RGB565 pixels on a
  white background. Only hosts seen in buffered visits are served; 404 when the
  site has no usable favicon. Tries `/favicon.ico` (PNG and BMP entries), the
  Google favicon service, then `/apple-touch-icon.png`, and caches the result
  in memory.
- `GET /api/flag.rgb565?cc=<country code>&w=<4-64>&h=<4-64>` — the country
  flag for a visit's `countryCode`, same raw format. Rendered from the 3:2 SVGs
  bundled in `country-flag-icons` (no network access); 404 for unknown codes.

On the first poll of a site the backend records a baseline (all current visits
are considered already seen), so only genuinely new visits are emitted
afterwards.

## Notification card

Shows site name, site favicon, number of actions, number of goal conversions,
country flag, device type (mobile/desktop) and visit start time. An optional
sound chime plays per new visit (toggle with the speaker button). Clicking the
card shows the previous notification; the dots below jump between the last 10.
