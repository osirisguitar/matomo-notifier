import "./style.css";
import type { VisitNotification } from "./types.js";
import { countryFlag, formatTime, playChime, setFavicon } from "./utils.js";

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

soundToggle.addEventListener("click", () => {
  soundOn = !soundOn;
  soundToggle.textContent = soundOn ? "🔊" : "🔇";
  soundToggle.setAttribute("aria-pressed", String(soundOn));
});

async function poll() {
  try {
    const res = await fetch(`/api/visits?after=${cursor}`);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data: { visits: VisitNotification[]; cursor: number } =
      await res.json();
    cursor = data.cursor;

    for (const visit of data.visits.reverse()) {
      if (history.some((v) => v.id === visit.id)) continue;
      history.unshift(visit);
      if (history.length > HISTORY_MAX) history.pop();
      viewIndex = 0;
      if (soundOn) playChime();
    }
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
  const key = `${history.map((v) => v.id).join(",")}|${viewIndex}`;
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
  const location =
    [v.city, v.country].filter(Boolean).join(", ") || "Unknown location";
  title.innerHTML = `
    <strong class="site">${escapeHtml(v.siteName)}</strong>
    <span class="meta">
      <span class="flag" title="${escapeHtml(v.country ?? "")}">${countryFlag(v.countryCode)}</span>
      <span class="city">${escapeHtml(location)}</span>
      <span class="device">${v.deviceType === "mobile" ? "Mobile" : "Desktop"}</span>
    </span>`;
  head.append(favicon, title);

  const body = document.createElement("div");
  body.className = "body";
  body.innerHTML = `
    <div class="hero">
      <span class="hero-value">${v.actions}</span><span class="hero-unit">${v.actions === 1 ? "action" : "actions"}</span>
    </div>
    <p class="hero-caption">New visit on ${escapeHtml(v.siteName)}.</p>
    <div class="stats">
      <div class="stat">
        <svg class="icon" viewBox="0 0 24 24" aria-hidden="true">
          <circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2.5"/>
          <path d="M12 7v5l3.5 2" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"/>
        </svg>
        <time class="value" datetime="${v.visitStart}">${formatTime(v.visitStart)}</time>
        <span class="label">Visit started</span>
      </div>
      <div class="stat">
        <svg class="icon" viewBox="0 0 24 24" aria-hidden="true">
          <circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2.5"/>
          <circle cx="12" cy="12" r="4.5" fill="none" stroke="currentColor" stroke-width="2.5"/>
          <circle cx="12" cy="12" r="1.5" fill="currentColor"/>
        </svg>
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
