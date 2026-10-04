import { eq } from "drizzle-orm";
import { Hono } from "hono";
import { HTTPException } from "hono/http-exception";
import type { Db } from "./db.ts";
import { clientIp, limit, minute } from "./limit.ts";
import { gameExecutables, games, robloxPlaces } from "./schema.ts";

export type Game = typeof games.$inferSelect;

// Roblox on Windows, on macOS and through Sober on Linux, as the app names their executables.
const robloxExecutables = ["robloxplayerbeta.exe", "robloxplayer", "sober"];

export function isRoblox(executable: string): boolean {
  return robloxExecutables.includes(executable);
}

// The app sends a file name. Anything up to a slash is dropped, so a full path names the same game.
export function executableName(text: string): string {
  return (text.split(/[\\/]/).at(-1) ?? "").trim().toLowerCase();
}

export function placeId(text: string | undefined): number | undefined {
  if (!text) return undefined;
  if (!/^\d{1,15}$/.test(text)) throw new HTTPException(400, { message: "The place ID must be a number." });
  return Number(text);
}

export async function ensureRoblox(db: Db) {
  if (await gameOf(db, robloxExecutables[0])) return;
  await db.transaction(async (tx) => {
    const [roblox] = await tx.insert(games).values({ name: "Roblox" }).returning();
    await tx.insert(gameExecutables).values(robloxExecutables.map((executable) => ({ executable, gameId: roblox.id })));
  });
}

export async function gameOf(db: Db, executable: string): Promise<Game | undefined> {
  const [row] = await db
    .select({ game: games })
    .from(gameExecutables)
    .innerJoin(games, eq(games.id, gameExecutables.gameId))
    .where(eq(gameExecutables.executable, executable));
  return row?.game;
}

export async function addGame(db: Db, executable: string, name: string): Promise<Game> {
  return db.transaction(async (tx) => {
    const [game] = await tx.insert(games).values({ name }).returning();
    await tx.insert(gameExecutables).values({ executable, gameId: game.id });
    return game;
  });
}

async function fromRoblox<T>(url: string): Promise<T> {
  const response = await fetch(url);
  if (!response.ok) {
    console.error(`Roblox answered ${response.status} for ${url}`);
    throw new HTTPException(502, { message: "Roblox didn't answer. Try again in a bit." });
  }
  return (await response.json()) as T;
}

// The experience a place belongs to, added the first time someone plays it. Undefined when Roblox doesn't know the
// place. Experiences are looked up by universe, since lobbies and matches are often different places.
export async function experienceOf(db: Db, roblox: Game, place: number): Promise<Game | undefined> {
  const [cached] = await db
    .select({ game: games })
    .from(robloxPlaces)
    .innerJoin(games, eq(games.id, robloxPlaces.gameId))
    .where(eq(robloxPlaces.placeId, place));
  if (cached) return cached.game;

  const { universeId } = await fromRoblox<{ universeId: number | null }>(`https://apis.roblox.com/universes/v1/places/${place}/universe`);
  if (!universeId) return undefined;
  const { data } = await fromRoblox<{ data: { id: number; name: string }[] }>(`https://games.roblox.com/v1/games?universeIds=${universeId}`);
  const name = data.find((game) => game.id === universeId)?.name;
  if (!name) return undefined;

  const [experience] = await db
    .insert(games)
    .values({ parentId: roblox.id, name, robloxUniverseId: universeId })
    .onConflictDoUpdate({ target: games.robloxUniverseId, set: { name } })
    .returning();
  await db.insert(robloxPlaces).values({ placeId: place, gameId: experience.id }).onConflictDoNothing();
  return experience;
}

export function gameRoutes(db: Db) {
  return new Hono().get("/games/resolve", async (c) => {
    limit(`resolve:${clientIp(c)}`, 60, minute);
    const executable = executableName(c.req.query("executable") ?? "");
    const place = placeId(c.req.query("place"));
    const game = executable ? await gameOf(db, executable) : undefined;
    if (!game) return c.json({ game: null, experience: null });

    let experience: Game | undefined;
    if (place && isRoblox(executable)) {
      try {
        experience = await experienceOf(db, game, place);
      } catch (error) {
        // Browsing falls back to all of Roblox while Roblox's API is down.
        if (!(error instanceof HTTPException)) throw error;
      }
    }
    return c.json({
      game: { id: game.id, name: game.name },
      experience: experience ? { id: experience.id, name: experience.name } : null,
    });
  });
}
