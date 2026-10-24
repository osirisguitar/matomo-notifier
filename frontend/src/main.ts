import "./style.css";
import type { VisitNotification } from "./types.js";
import { formatTime, playChime, setFavicon } from "./utils.js";

const HISTORY_MAX = 10;
const POLL_MS = 2000;

const cardArea = document.querySelector<HTMLDivElement>("#card-area")!;
const statusEl = document.querySelector<HTMLSpanElement>("#status")!;
const soundToggle = document.querySelector<HTMLButtonElement>("#sound-toggle")!;
const dotsEl = document.querySelector<HTMLElement>("#history-dots")!;

let cursor = 0;
let history: VisitNotification[] = []; // index 0 = newest
let viewIndex = 0;
let soundOn = true;
let lastRenderKey = "";
// The first successful poll loads visits detected before the page opened:
// show them, but don't chime for them
let primed = false;

soundToggle.addEventListener("click", () => {
  soundOn = !soundOn;
  soundToggle.textContent = soundOn ? "🔊" : "🔇";
  soundToggle.setAttribute("aria-pressed", String(soundOn));
});

async function poll() {
  try {
    const res = await fetch(`/api/visits?after=${cursor}&limit=${HISTORY_MAX}`);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data: { visits: VisitNotification[]; cursor: number } =
      await res.json();
    cursor = data.cursor;

    // Oldest first, so the newest ends up at index 0. Visits we already have
    // are updates (Matomo keeps counting actions while the visitor browses):
    // replace them in place, without a chime or jumping to them.
    let fresh = 0;
    for (const visit of data.visits) {
      const known = history.findIndex((v) => v.id === visit.id);
      if (known >= 0) {
        history[known] = visit;
        continue;
      }
      history.unshift(visit);
      if (history.length > HISTORY_MAX) history.pop();
      viewIndex = 0;
      fresh++;
    }
    if (fresh > 0 && primed && soundOn) playChime();
    primed = true;
    statusEl.textContent = "connected";
    statusEl.classList.remove("error");
  } catch {
    statusEl.textContent = "backend unreachable";
    statusEl.classList.add("error");
  }
  render();
}

function render() {
  // Re-rendering resets the card's CSS animation and refetches the favicon,
  // which shows up as flicker. Skip renders when nothing changed.
  const key = `${history.map((v) => `${v.id}:${v.actions}:${v.goals}`).join(",")}|${viewIndex}`;
  if (key === lastRenderKey) return;
  lastRenderKey = key;

  cardArea.replaceChildren();
  dotsEl.replaceChildren();

  const visit = history[viewIndex];
  if (visit) {
    cardArea.append(card(visit));
  } else {
    cardArea.append(empty());
  }

  history.forEach((v, i) => {
    const dot = document.createElement("button");
    dot.type = "button";
    dot.className = "dot" + (i === viewIndex ? " active" : "");
    dot.title = `${v.siteName} — ${formatTime(v.visitStart)}`;
    dot.addEventListener("click", () => {
      viewIndex = i;
      render();
    });
    dotsEl.append(dot);
  });
}

function empty(): HTMLElement {
  const el = document.createElement("div");
  el.className = "empty";
  el.textContent = "Waiting for visits…";
  return el;
}

function card(v: VisitNotification): HTMLElement {
  const el = document.createElement("article");
  el.className = "card";
  el.title = "Click to see the previous notification";

  const favicon = document.createElement("img");
  favicon.className = "favicon";
  favicon.alt = "";
  setFavicon(favicon, v.host, v.siteName);

  const head = document.createElement("header");
  head.className = "card-head";
  const title = document.createElement("div");
  title.className = "title";
  // Like the CYD: "<flag> <city> · <device>", the flag standing in for the
  // country name (kept as its tooltip); "<city, country>" without a flag
  const fullLocation =
    [v.city, v.country].filter(Boolean).join(", ") || "Unknown location";
  const hasFlag = /^[a-z]{2}$/.test(v.countryCode) && v.countryCode !== "xx";
  const location = hasFlag && v.city ? v.city : fullLocation;
  title.innerHTML = `
    <strong class="site">${escapeHtml(v.siteName)}</strong>
    <span class="meta">
      ${hasFlag ? `<img class="flag" src="/api/flag.svg?cc=${v.countryCode}" alt="" title="${escapeHtml(v.country ?? "")}">` : ""}
      <span class="city">${escapeHtml(location)}</span>
      <span class="device">${v.deviceType === "mobile" ? "Mobile" : "Desktop"}</span>
    </span>`;
  title.querySelector<HTMLImageElement>(".flag")?.addEventListener("error", (e) => {
    (e.target as HTMLElement).remove();
    title.querySelector(".city")!.textContent = fullLocation;
  });
  head.append(favicon, title);

  // Title of the latest page visited (cut off with an ellipsis by CSS)
  const caption = v.lastPageTitle || `New visit on ${v.siteName}.`;
  const body = document.createElement("div");
  body.className = "body";
  body.innerHTML = `
    <div class="hero">${v.actions} ${v.actions === 1 ? "action" : "actions"}</div>
    <p class="hero-caption" title="${escapeHtml(caption)}">${escapeHtml(caption)}</p>
    <div class="stats">
      <div class="stat">
        <time class="value" datetime="${v.visitStart}">${formatTime(v.visitStart)}</time>
        <span class="label">Visit started</span>
      </div>
      <div class="stat">
        <span class="value">${v.goals}</span>
        <span class="label">Goal ${v.goals === 1 ? "conversion" : "conversions"}</span>
      </div>
    </div>`;

  el.append(head, body);

  el.addEventListener("click", () => {
    if (viewIndex + 1 < history.length) {
      viewIndex++;
      render();
    }
  });
  return el;
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]!,
  );
}

setInterval(poll, POLL_MS);
void poll();
