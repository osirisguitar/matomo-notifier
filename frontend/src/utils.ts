export function formatTime(iso: string): string {
  // 24-hour HH:MM like the CYD, whatever the browser locale
  return new Date(iso).toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
    hourCycle: "h23",
  });
}

export function setFavicon(img: HTMLImageElement, host: string | undefined, siteName: string): void {
  if (!host) return placeholder(img, siteName);
  const candidates = [
    `https://${host}/favicon.ico`,
    `https://www.google.com/s2/favicons?domain=${host}&sz=64`,
    `https://${host}/apple-touch-icon.png`,
  ];
  let i = 0;
  img.onerror = () => {
    i++;
    if (i < candidates.length) {
      img.src = candidates[i];
    } else {
      placeholder(img, siteName);
    }
  };
  img.src = candidates[0];
}

function placeholder(img: HTMLImageElement, siteName: string): void {
  img.onerror = null;
  const letter = (siteName[0] ?? "?").toUpperCase();
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">` +
    `<rect width="32" height="32" rx="6" fill="#4f8cff"/>` +
    `<text x="16" y="22" font-size="16" font-family="sans-serif" fill="#fff" text-anchor="middle">${letter}</text>` +
    `</svg>`;
  img.src = `data:image/svg+xml,${encodeURIComponent(svg)}`;
}

export function playChime(): void {
  const ctx = new AudioContext();
  const osc = ctx.createOscillator();
  const gain = ctx.createGain();
  osc.type = "sine";
  osc.frequency.setValueAtTime(880, ctx.currentTime);
  osc.frequency.setValueAtTime(1320, ctx.currentTime + 0.12);
  gain.gain.setValueAtTime(0.15, ctx.currentTime);
  gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + 0.4);
  osc.connect(gain).connect(ctx.destination);
  osc.start();
  osc.stop(ctx.currentTime + 0.4);
}
