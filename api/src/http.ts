import type { Context } from "hono";
import { HTTPException } from "hono/http-exception";

export async function readJson(c: Context): Promise<Record<string, unknown>> {
  const body: unknown = await c.req.json().catch(() => null);
  if (typeof body !== "object" || body === null || Array.isArray(body)) throw new HTTPException(400, { message: "The request must be a JSON object." });
  return Object.fromEntries(Object.entries(body));
}

// A text field from a JSON body or a form, trimmed. Empty counts as missing.
export function textField(value: unknown, label: string, max: number): string | undefined {
  if (value === undefined || value === null) return undefined;
  if (typeof value !== "string") throw new HTTPException(400, { message: `${label} must be text.` });
  const text = value.trim();
  if (text.length > max) throw new HTTPException(400, { message: `${label} can be at most ${max} characters.` });
  return text || undefined;
}
