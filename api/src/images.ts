import { HTTPException } from "hono/http-exception";
import sharp from "sharp";

const maxPixels = 4096 * 4096;

export const imageNames = ["before", "after", "thumbnail"] as const;

export function imageKey(presetId: number, name: (typeof imageNames)[number]): string {
  return `presets/${presetId}/${name}.jpg`;
}

async function size(image: Uint8Array): Promise<{ width: number; height: number }> {
  let metadata;
  try {
    metadata = await sharp(image).metadata();
  } catch {
    throw new HTTPException(400, { message: "Screenshots must be JPEG or PNG images." });
  }
  if (metadata.format !== "jpeg" && metadata.format !== "png") throw new HTTPException(400, { message: "Screenshots must be JPEG or PNG images." });
  if (metadata.width * metadata.height > maxPixels) throw new HTTPException(400, { message: "The screenshots are too large." });
  return { width: metadata.width, height: metadata.height };
}

// Copied out of the Buffer, since fetch bodies need a plain ArrayBuffer behind them.
async function jpeg(image: Uint8Array, width: number, height: number, quality: number): Promise<Uint8Array<ArrayBuffer>> {
  const output = await sharp(image, { limitInputPixels: maxPixels }).resize({ width, height, fit: "inside", withoutEnlargement: true }).jpeg({ quality }).toBuffer();
  return new Uint8Array(output);
}

// Re-encodes what the app sent, so the apps that download a preset only ever decode JPEGs made here.
export async function processScreenshots(before: Uint8Array, after: Uint8Array): Promise<Record<(typeof imageNames)[number], Uint8Array<ArrayBuffer>>> {
  const [beforeSize, afterSize] = await Promise.all([size(before), size(after)]);
  if (beforeSize.width !== afterSize.width || beforeSize.height !== afterSize.height)
    throw new HTTPException(400, { message: "The before and after screenshots must be the same size." });
  const [beforeJpeg, afterJpeg, thumbnail] = await Promise.all([jpeg(before, 1920, 1080, 85), jpeg(after, 1920, 1080, 85), jpeg(after, 480, 270, 80)]);
  return { before: beforeJpeg, after: afterJpeg, thumbnail };
}
