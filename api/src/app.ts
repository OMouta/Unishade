import { Hono } from "hono";
import { HTTPException } from "hono/http-exception";
import { authRoutes } from "./auth.ts";
import type { Db } from "./db.ts";
import { gameRoutes } from "./games.ts";
import { moderationRoutes } from "./moderation.ts";
import { presetRoutes } from "./presets.ts";

export function createApp(db: Db) {
  const app = new Hono();
  app.onError((error, c) => {
    if (error instanceof HTTPException) return c.json({ error: error.message }, error.status);
    console.error(error);
    return c.json({ error: "Something went wrong on Unishade's side. Try again in a bit." }, 500);
  });
  app.route("/", authRoutes(db));
  app.route("/", gameRoutes(db));
  app.route("/", presetRoutes(db));
  app.route("/", moderationRoutes(db));
  return app;
}
