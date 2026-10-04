import { and, arrayContains, count, desc, eq, sql } from "drizzle-orm";
import { Hono } from "hono";
import { bodyLimit } from "hono/body-limit";
import { HTTPException } from "hono/http-exception";
import { type SignedIn, requireUser } from "./auth.ts";
import { deleteObjects, getObject, putObject, signedUrl } from "./bucket.ts";
import type { Db } from "./db.ts";
import { postMessage } from "./discord.ts";
import { env } from "./env.ts";
import { addGame, executableName, experienceOf, gameOf, isRoblox, placeId } from "./games.ts";
import { readJson, textField } from "./http.ts";
import { imageKey, imageNames, processScreenshots } from "./images.ts";
import { dlss5Settings, presetEffects } from "./ini.ts";
import { allow, clientIp, day, limit } from "./limit.ts";
import { reportMessage, reviewMessage } from "./moderation.ts";
import { games, presets, reports, users } from "./schema.ts";

const pageSize = 30;
const maxPending = 5;

// What lists show. The ini only comes with a single preset.
const listed = {
  id: presets.id,
  name: presets.name,
  description: presets.description,
  effects: presets.effects,
  needsDepth: presets.needsDepth,
  dlss5: presets.dlss5,
  saves: presets.saves,
  createdAt: presets.createdAt,
  author: users.name,
};

function presetId(text: string): number {
  const id = Number(text);
  if (!Number.isSafeInteger(id) || id < 1) throw new HTTPException(404, { message: "That preset doesn't exist." });
  return id;
}

async function withThumbnail<T extends { id: number }>(row: T) {
  return { ...row, thumbnail: await signedUrl(imageKey(row.id, "thumbnail")) };
}

// "Jailbreak (Roblox)" for an experience, the game's name otherwise.
async function gameLabel(db: Db, gameId: number): Promise<string> {
  const [game] = await db.select().from(games).where(eq(games.id, gameId));
  if (!game.parentId) return game.name;
  const [parent] = await db.select().from(games).where(eq(games.id, game.parentId));
  return `${game.name} (${parent.name})`;
}

export function presetRoutes(db: Db) {
  const signedIn = requireUser(db);
  return new Hono<SignedIn>()
    .get("/presets", async (c) => {
      const game = Number(c.req.query("game"));
      if (!Number.isSafeInteger(game)) throw new HTTPException(400, { message: "Pick a game." });
      const effect = c.req.query("effect");
      const offset = Math.max(0, Math.trunc(Number(c.req.query("offset"))) || 0);
      const order = c.req.query("sort") === "new" ? [desc(presets.createdAt)] : [desc(presets.saves), desc(presets.id)];
      const rows = await db
        .select(listed)
        .from(presets)
        .innerJoin(users, eq(users.id, presets.userId))
        .where(and(eq(presets.gameId, game), eq(presets.status, "approved"), effect ? arrayContains(presets.effects, [effect]) : undefined))
        .orderBy(...order)
        .limit(pageSize + 1)
        .offset(offset);
      return c.json({
        presets: await Promise.all(rows.slice(0, pageSize).map(withThumbnail)),
        next: rows.length > pageSize ? offset + pageSize : null,
      });
    })
    .get("/presets/:id", async (c) => {
      const id = presetId(c.req.param("id"));
      const [row] = await db
        .select({ ...listed, ini: presets.ini, game: { id: games.id, name: games.name } })
        .from(presets)
        .innerJoin(users, eq(users.id, presets.userId))
        .innerJoin(games, eq(games.id, presets.gameId))
        .where(and(eq(presets.id, id), eq(presets.status, "approved")));
      if (!row) throw new HTTPException(404, { message: "That preset doesn't exist." });
      return c.json({
        ...(await withThumbnail(row)),
        before: await signedUrl(imageKey(id, "before")),
        after: await signedUrl(imageKey(id, "after")),
      });
    })
    // Counts someone keeping a preset, once a day for each address.
    .post("/presets/:id/saves", async (c) => {
      const id = presetId(c.req.param("id"));
      if (allow(`save:${clientIp(c)}:${id}`, 1, day))
        await db
          .update(presets)
          .set({ saves: sql`${presets.saves} + 1` })
          .where(and(eq(presets.id, id), eq(presets.status, "approved")));
      return c.body(null, 204);
    })
    .post(
      "/presets",
      bodyLimit({ maxSize: 32 * 1024 * 1024, onError: (c) => c.json({ error: "The screenshots are too large." }, 413) }),
      signedIn,
      async (c) => {
        const user = c.get("user");
        const form = await c.req.parseBody();
        const name = textField(form.name, "The name", 40);
        if (!name) throw new HTTPException(400, { message: "Give the preset a name." });
        const description = textField(form.description, "The description", 500) ?? "";
        const executable = executableName(textField(form.executable, "The game", 260) ?? "");
        if (!executable) throw new HTTPException(400, { message: "Pick the game the preset is for." });
        const place = placeId(textField(form.place, "The place ID", 20));
        if (place && !isRoblox(executable)) throw new HTTPException(400, { message: "Only Roblox presets can be for an experience." });
        // Forms send line breaks as CRLF. ReShade reads either.
        const ini = textField(form.ini, "The preset", 64 * 1024)?.replace(/\r\n/g, "\n");
        if (!ini) throw new HTTPException(400, { message: "Send the preset's ini." });
        const effects = presetEffects(ini);
        const dlss5Text = textField(form.dlss5, "The DLSS5 settings", 2000);
        const dlss5 = dlss5Text ? dlss5Settings(dlss5Text) : null;
        if (!(form.before instanceof File) || !(form.after instanceof File))
          throw new HTTPException(400, { message: "Send the before and after screenshots." });

        const [{ pending }] = await db
          .select({ pending: count() })
          .from(presets)
          .where(and(eq(presets.userId, user.id), eq(presets.status, "pending")));
        if (pending >= maxPending)
          throw new HTTPException(429, { message: `You have ${maxPending} presets waiting for review. Publish more once they're reviewed.` });

        // Counted here so a form the API turned away doesn't use up the day.
        limit(`publish:${user.id}`, 10, day);
        const images = await processScreenshots(new Uint8Array(await form.before.arrayBuffer()), new Uint8Array(await form.after.arrayBuffer()));

        let game = await gameOf(db, executable);
        if (!game) {
          const gameName = textField(form.gameName, "The game's name", 60);
          if (!gameName) throw new HTTPException(400, { message: "Name the game to publish the first preset for it." });
          game = await addGame(db, executable, gameName);
        }
        if (place) {
          game = await experienceOf(db, game, place);
          if (!game) throw new HTTPException(400, { message: "Roblox doesn't know that experience." });
        }

        const [preset] = await db
          .insert(presets)
          .values({ gameId: game.id, userId: user.id, name, description, effects, ini, needsDepth: form.needsDepth === "true", dlss5 })
          .returning();
        try {
          await Promise.all(imageNames.map((image) => putObject(imageKey(preset.id, image), images[image], "image/jpeg")));
          await postMessage(env.reviewChannelId, reviewMessage(preset, await gameLabel(db, game.id)), [
            { name: "before.jpg", data: images.before },
            { name: "after.jpg", data: images.after },
          ]);
        } catch (error) {
          await db.delete(presets).where(eq(presets.id, preset.id));
          await deleteObjects(imageNames.map((image) => imageKey(preset.id, image)));
          throw error;
        }
        return c.json({ id: preset.id, status: preset.status }, 201);
      },
    )
    .delete("/presets/:id", signedIn, async (c) => {
      const id = presetId(c.req.param("id"));
      const [deleted] = await db
        .delete(presets)
        .where(and(eq(presets.id, id), eq(presets.userId, c.get("user").id)))
        .returning({ id: presets.id });
      if (!deleted) throw new HTTPException(404, { message: "You don't have a preset with that ID." });
      await deleteObjects(imageNames.map((image) => imageKey(id, image)));
      return c.body(null, 204);
    })
    .post("/presets/:id/reports", signedIn, async (c) => {
      const user = c.get("user");
      const id = presetId(c.req.param("id"));
      limit(`report:${user.id}`, 10, day);
      const reason = textField((await readJson(c)).reason, "The reason", 300);
      if (!reason) throw new HTTPException(400, { message: "Say what's wrong with the preset." });
      const [preset] = await db
        .select()
        .from(presets)
        .where(and(eq(presets.id, id), eq(presets.status, "approved")));
      if (!preset) throw new HTTPException(404, { message: "That preset doesn't exist." });

      // Reporting the same preset again doesn't post again.
      const [report] = await db.insert(reports).values({ presetId: id, userId: user.id, reason }).onConflictDoNothing().returning();
      if (!report) return c.body(null, 204);
      try {
        const after = await getObject(imageKey(id, "after"));
        await postMessage(env.reviewChannelId, reportMessage(preset, await gameLabel(db, preset.gameId), reason, user.id), [
          { name: "after.jpg", data: after },
        ]);
      } catch (error) {
        await db.delete(reports).where(and(eq(reports.presetId, id), eq(reports.userId, user.id)));
        throw error;
      }
      return c.body(null, 204);
    })
    // The signed-in person's presets with their review status, so they can see why one was rejected.
    .get("/me/presets", signedIn, async (c) => {
      const rows = await db
        .select({
          id: presets.id,
          name: presets.name,
          status: presets.status,
          rejectReason: presets.rejectReason,
          saves: presets.saves,
          createdAt: presets.createdAt,
          game: games.name,
        })
        .from(presets)
        .innerJoin(games, eq(games.id, presets.gameId))
        .where(eq(presets.userId, c.get("user").id))
        .orderBy(desc(presets.createdAt));
      // Rejecting a preset deletes its screenshots.
      const withThumbnails = rows.map(async (row) =>
        row.status === "rejected" ? { ...row, thumbnail: null } : withThumbnail(row),
      );
      return c.json({ presets: await Promise.all(withThumbnails) });
    });
}
