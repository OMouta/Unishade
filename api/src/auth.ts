import { createHash, randomBytes } from "node:crypto";
import { and, eq, gt, lt } from "drizzle-orm";
import { Hono } from "hono";
import { createMiddleware } from "hono/factory";
import { HTTPException } from "hono/http-exception";
import type { Db } from "./db.ts";
import { authorizeUrl, signedInUser } from "./discord.ts";
import { readJson, textField } from "./http.ts";
import { clientIp, limit, minute } from "./limit.ts";
import { logins, sessions, users } from "./schema.ts";

export type SignedIn = { Variables: { user: typeof users.$inferSelect } };

const hash = (text: string) => createHash("sha256").update(text).digest("hex");
const secret = () => randomBytes(32).toString("base64url");
// Sign-ins started before this are expired.
const loginCutoff = () => new Date(Date.now() - 10 * minute);

function bearer(header: string | undefined): string | undefined {
  return header?.match(/^Bearer (\S+)$/)?.[1];
}

export function requireUser(db: Db) {
  return createMiddleware<SignedIn>(async (c, next) => {
    const token = bearer(c.req.header("authorization"));
    const [row] = token
      ? await db.select({ user: users }).from(sessions).innerJoin(users, eq(users.id, sessions.userId)).where(eq(sessions.tokenHash, hash(token)))
      : [];
    if (!row) throw new HTTPException(401, { message: "Sign in with Discord first." });
    if (row.user.bannedAt) throw new HTTPException(403, { message: "Your account can't publish or report presets." });
    c.set("user", row.user);
    await next();
  });
}

// The app starts a sign-in, opens the link in the browser, and polls with its secret until Discord's redirect
// lands on /auth/callback.
export function authRoutes(db: Db) {
  return new Hono<SignedIn>()
    .post("/auth/start", async (c) => {
      limit(`login:${clientIp(c)}`, 10, minute);
      await db.delete(logins).where(lt(logins.createdAt, loginCutoff()));
      const id = secret();
      const poll = secret();
      await db.insert(logins).values({ id, pollHash: hash(poll) });
      return c.json({ url: authorizeUrl(id), poll });
    })
    .get("/auth/callback", async (c) => {
      const page = (text: string, status: 200 | 400 = 200) =>
        c.html(`<!doctype html><meta charset="utf-8"><title>Unishade</title><p>${text}</p>`, status);
      const { code, state, error } = c.req.query();
      if (error) return page("Sign-in was cancelled. You can close this tab.");
      const [login] = state ? await db.select().from(logins).where(and(eq(logins.id, state), gt(logins.createdAt, loginCutoff()))) : [];
      if (!login || !code) return page("This sign-in link expired. Sign in again from Unishade.", 400);

      const user = await signedInUser(code);
      await db.insert(users).values(user).onConflictDoUpdate({ target: users.id, set: { name: user.name } });
      await db.update(logins).set({ userId: user.id }).where(eq(logins.id, login.id));
      return page("Signed in. You can go back to your game.");
    })
    .post("/auth/poll", async (c) => {
      limit(`poll:${clientIp(c)}`, 120, minute);
      const poll = textField((await readJson(c)).poll, "poll", 100);
      const [login] = poll ? await db.select().from(logins).where(and(eq(logins.pollHash, hash(poll)), gt(logins.createdAt, loginCutoff()))) : [];
      if (!login) throw new HTTPException(410, { message: "The sign-in expired. Start again." });
      if (!login.userId) return c.body(null, 202);

      // Deleting it first means two polls at once can't both get a session.
      const [claimed] = await db.delete(logins).where(eq(logins.id, login.id)).returning();
      if (!claimed) throw new HTTPException(410, { message: "The sign-in expired. Start again." });
      const token = secret();
      await db.insert(sessions).values({ tokenHash: hash(token), userId: login.userId });
      const [user] = await db.select().from(users).where(eq(users.id, login.userId));
      return c.json({ token, user: { id: user.id, name: user.name } });
    })
    .post("/auth/logout", async (c) => {
      const token = bearer(c.req.header("authorization"));
      if (token) await db.delete(sessions).where(eq(sessions.tokenHash, hash(token)));
      return c.body(null, 204);
    })
    .get("/me", requireUser(db), (c) => {
      const user = c.get("user");
      return c.json({ id: user.id, name: user.name });
    });
}
