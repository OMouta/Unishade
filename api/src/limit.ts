import type { Context } from "hono";
import { HTTPException } from "hono/http-exception";

export const minute = 60_000;
export const day = 24 * 60 * minute;

// Counts in fixed windows, in memory: the API runs as a single instance.
const windows = new Map<string, { count: number; resets: number }>();

export function allow(key: string, max: number, ms: number): boolean {
  const now = Date.now();
  if (windows.size > 50_000) for (const [known, window] of windows) if (window.resets <= now) windows.delete(known);
  const window = windows.get(key);
  if (!window || window.resets <= now) windows.set(key, { count: 1, resets: now + ms });
  else if (window.count >= max) return false;
  else window.count++;
  return true;
}

export function limit(key: string, max: number, ms: number) {
  if (!allow(key, max, ms)) throw new HTTPException(429, { message: "Too many requests. Try again later." });
}

// Railway's proxy puts the client's address in X-Real-IP.
export function clientIp(c: Context): string {
  return c.req.header("x-real-ip") ?? "local";
}
