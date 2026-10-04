import { serve } from "@hono/node-server";
import { drizzle } from "drizzle-orm/postgres-js";
import postgres from "postgres";
import { createApp } from "./app.ts";
import { env } from "./env.ts";
import { ensureRoblox } from "./games.ts";

const db = drizzle(postgres(env.databaseUrl));
await ensureRoblox(db);

const port = Number(process.env.PORT ?? 3000);
serve({ fetch: createApp(db).fetch, port }, () => console.log(`Listening on port ${port}`));
