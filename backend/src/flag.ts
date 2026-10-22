import sharp from "sharp";
import * as flags from "country-flag-icons/string/3x2";
import { toRgb565 } from "./bitmap.js";

// Country flags (the web shows them as emoji, see countryFlag() in the
// frontend's utils.ts) as RGB565 bitmaps for the CYD, rendered from the 3:2
// SVGs bundled in country-flag-icons, so no network access is needed

const svgs = flags as Record<string, string>;
const cache = new Map<string, Promise<Buffer>>();

/** The flag's SVG source (for the web card), or null for unknown codes */
export function flagSvg(countryCode: string): string | null {
  return (/^[a-z]{2}$/.test(countryCode) && svgs[countryCode.toUpperCase()]) || null;
}

/** null when there's no flag for the code (Matomo uses "xx" for unknown) */
export function flagRgb565(countryCode: string, width: number, height: number): Promise<Buffer> | null {
  const svg = flagSvg(countryCode);
  if (!svg) return null;
  const key = `${countryCode}@${width}x${height}`;
  let bitmap = cache.get(key);
  if (!bitmap) {
    // "cover": the SVGs are all 3:2, so this only trims rounding at odd sizes
    bitmap = toRgb565(sharp(Buffer.from(svg), { density: 300 }), width, height, "cover");
    cache.set(key, bitmap);
  }
  return bitmap;
}
