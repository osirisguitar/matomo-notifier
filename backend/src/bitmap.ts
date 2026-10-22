import type { Sharp } from "sharp";

export const BITMAP_SIZE_MIN = 4;
export const BITMAP_SIZE_MAX = 64;

/**
 * Raw bitmap for the CYD display (cyd/src/main.cpp pushes it straight to the
 * screen with pushImage): width x height pixels, row-major, one
 * little-endian uint16 RGB565 value per pixel, transparency flattened onto
 * white.
 */
export async function toRgb565(
  image: Sharp,
  width: number,
  height: number,
  fit: "contain" | "cover" = "contain",
): Promise<Buffer> {
  const { data } = await image
    .resize(width, height, { fit, background: "#ffffff" })
    .flatten({ background: "#ffffff" })
    .removeAlpha()
    .raw()
    .toBuffer({ resolveWithObject: true });
  const out = Buffer.alloc(width * height * 2);
  for (let i = 0; i < width * height; i++) {
    const [r, g, b] = [data[i * 3], data[i * 3 + 1], data[i * 3 + 2]];
    out.writeUInt16LE(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), i * 2);
  }
  return out;
}
