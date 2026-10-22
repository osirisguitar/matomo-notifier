import sharp, { type Sharp } from "sharp";
import { toRgb565 } from "./bitmap.js";

// Converts a site's favicon into a raw RGB565 bitmap for the CYD display
// (format: see bitmap.ts).
// Same candidate order as the frontend's setFavicon() in utils.ts.

const FETCH_TIMEOUT_MS = 5_000;
const MAX_BYTES = 1_000_000;
const CACHE_OK_MS = 24 * 60 * 60 * 1000;
const CACHE_MISS_MS = 60 * 60 * 1000;
const CACHE_MAX = 200;

const cache = new Map<string, { expires: number; bitmap: Promise<Buffer | null> }>();

/**
 * size x size RGB565 bitmap on white (the web shows favicons on a white
 * tile). Resolves to null when the site has no usable favicon.
 */
export function faviconRgb565(host: string, size: number): Promise<Buffer | null> {
  const key = `${host}@${size}`;
  const hit = cache.get(key);
  if (hit && hit.expires > Date.now()) return hit.bitmap;

  const bitmap = loadFavicon(host, size);
  const entry = { expires: Date.now() + CACHE_OK_MS, bitmap };
  cache.set(key, entry);
  void bitmap.then((b) => {
    if (!b) entry.expires = Date.now() + CACHE_MISS_MS;
  });
  if (cache.size > CACHE_MAX) cache.delete(cache.keys().next().value!);
  return bitmap;
}

async function loadFavicon(host: string, size: number): Promise<Buffer | null> {
  const candidates = [
    `https://${host}/favicon.ico`,
    `https://www.google.com/s2/favicons?domain=${encodeURIComponent(host)}&sz=64`,
    `https://${host}/apple-touch-icon.png`,
  ];
  for (const url of candidates) {
    try {
      const data = await fetchBytes(url);
      if (!data) continue;
      return await toRgb565(await decode(data, size), size, size);
    } catch (err) {
      console.warn(`[favicon] ${url}: ${(err as Error).message}`);
    }
  }
  return null;
}

async function fetchBytes(url: string): Promise<Buffer | null> {
  const res = await fetch(url, {
    signal: AbortSignal.timeout(FETCH_TIMEOUT_MS),
    headers: { "user-agent": "matomo-notifier" },
  });
  if (!res.ok) return null;
  if (Number(res.headers.get("content-length") ?? 0) > MAX_BYTES) return null;
  const data = Buffer.from(await res.arrayBuffer());
  return data.length > 0 && data.length <= MAX_BYTES ? data : null;
}

// ICO isn't supported by sharp, everything else (PNG, GIF, JPEG, WebP, SVG) is
function decode(data: Buffer, size: number): Sharp {
  const isIco = data.length > 6 && data.readUInt16LE(0) === 0 && data.readUInt16LE(2) === 1;
  if (!isIco) return sharp(data, { density: 300 });
  return decodeIco(data, size);
}

function decodeIco(data: Buffer, size: number): Sharp {
  const count = data.readUInt16LE(4);
  const entries = [];
  for (let i = 0; i < count; i++) {
    const at = 6 + i * 16;
    if (at + 16 > data.length) break;
    entries.push({
      width: data[at] || 256,
      length: data.readUInt32LE(at + 8),
      offset: data.readUInt32LE(at + 12),
    });
  }
  // Smallest image that is at least the target size, else the largest one
  entries.sort((a, b) => a.width - b.width);
  const entry = entries.find((e) => e.width >= size) ?? entries.at(-1);
  if (!entry || entry.offset + entry.length > data.length) throw new Error("bad ICO directory");
  const image = data.subarray(entry.offset, entry.offset + entry.length);

  if (image.readUInt32BE(0) === 0x89504e47) return sharp(image); // PNG entry
  return decodeIcoBmp(image);
}

// BMP-style ICO entry: BITMAPINFOHEADER, optional palette, bottom-up XOR
// pixels, then a 1bpp AND (transparency) mask for formats without alpha
function decodeIcoBmp(bmp: Buffer): Sharp {
  const headerSize = bmp.readUInt32LE(0);
  const width = bmp.readInt32LE(4);
  const height = bmp.readInt32LE(8) / 2; // XOR + AND mask
  const bpp = bmp.readUInt16LE(14);
  if (width <= 0 || height <= 0 || width > 256 || height > 256) throw new Error("bad ICO size");
  if (![1, 4, 8, 24, 32].includes(bpp)) throw new Error(`unsupported ICO depth ${bpp}`);

  const paletteSize = bpp <= 8 ? (bmp.readUInt32LE(32) || 1 << bpp) : 0;
  const paletteAt = headerSize;
  const pixelsAt = paletteAt + paletteSize * 4;
  const stride = Math.ceil((width * bpp) / 32) * 4;
  const maskAt = pixelsAt + stride * height;
  const maskStride = Math.ceil(width / 32) * 4;
  const hasMask = bpp !== 32 && maskAt + maskStride * height <= bmp.length;
  if (pixelsAt + stride * height > bmp.length) throw new Error("truncated ICO bitmap");

  const rgba = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y++) {
    const row = pixelsAt + (height - 1 - y) * stride;
    for (let x = 0; x < width; x++) {
      let b: number, g: number, r: number, a = 255;
      if (bpp === 32 || bpp === 24) {
        const p = row + x * (bpp / 8);
        [b, g, r] = [bmp[p], bmp[p + 1], bmp[p + 2]];
        if (bpp === 32) a = bmp[p + 3];
      } else {
        const bit = x * bpp;
        const index = (bmp[row + (bit >> 3)] >> (8 - bpp - (bit & 7))) & ((1 << bpp) - 1);
        const p = paletteAt + index * 4;
        [b, g, r] = [bmp[p], bmp[p + 1], bmp[p + 2]];
      }
      if (hasMask) {
        const m = maskAt + (height - 1 - y) * maskStride;
        if ((bmp[m + (x >> 3)] >> (7 - (x & 7))) & 1) a = 0;
      }
      rgba.set([r, g, b, a], (y * width + x) * 4);
    }
  }
  return sharp(rgba, { raw: { width, height, channels: 4 } });
}
