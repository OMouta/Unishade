import { type AnyPgColumn, bigint, boolean, index, integer, jsonb, pgTable, primaryKey, serial, text, timestamp } from "drizzle-orm/pg-core";

export const users = pgTable("users", {
  id: text().primaryKey(), // Discord user ID
  name: text().notNull(),
  bannedAt: timestamp("banned_at", { withTimezone: true }),
  createdAt: timestamp("created_at", { withTimezone: true }).notNull().defaultNow(),
});

export const sessions = pgTable("sessions", {
  tokenHash: text("token_hash").primaryKey(), // sha256 of the token the app keeps
  userId: text("user_id").notNull().references(() => users.id, { onDelete: "cascade" }),
  createdAt: timestamp("created_at", { withTimezone: true }).notNull().defaultNow(),
});

// A sign-in in progress: the app polls with a secret only it has until Discord's redirect sets the user.
export const logins = pgTable("logins", {
  id: text().primaryKey(), // the OAuth state
  pollHash: text("poll_hash").notNull().unique(),
  userId: text("user_id").references(() => users.id, { onDelete: "cascade" }),
  createdAt: timestamp("created_at", { withTimezone: true }).notNull().defaultNow(),
});

// Roblox experiences are games whose parent is Roblox.
export const games = pgTable("games", {
  id: serial().primaryKey(),
  parentId: integer("parent_id").references((): AnyPgColumn => games.id),
  name: text().notNull(),
  robloxUniverseId: bigint("roblox_universe_id", { mode: "number" }).unique(),
});

export const gameExecutables = pgTable("game_executables", {
  executable: text().primaryKey(), // lowercase file name, such as robloxplayerbeta.exe
  gameId: integer("game_id").notNull().references(() => games.id),
});

// Which experience a place belongs to, as Roblox's API answered.
export const robloxPlaces = pgTable("roblox_places", {
  placeId: bigint("place_id", { mode: "number" }).primaryKey(),
  gameId: integer("game_id").notNull().references(() => games.id),
});

export const presets = pgTable(
  "presets",
  {
    id: integer().primaryKey().generatedAlwaysAsIdentity(),
    gameId: integer("game_id").notNull().references(() => games.id),
    userId: text("user_id").notNull().references(() => users.id),
    name: text().notNull(),
    description: text().notNull().default(""),
    effects: text().array().notNull(),
    ini: text().notNull(),
    needsDepth: boolean("needs_depth").notNull().default(false),
    // The [RENODX-DLSS-preset1] keys when DLSS5 was on, null when it was off.
    dlss5: jsonb().$type<Record<string, string>>(),
    status: text({ enum: ["pending", "approved", "rejected"] }).notNull().default("pending"),
    rejectReason: text("reject_reason"),
    saves: integer().notNull().default(0),
    createdAt: timestamp("created_at", { withTimezone: true }).notNull().defaultNow(),
  },
  (t) => [index("presets_browse").on(t.gameId, t.status, t.saves.desc()), index("presets_effects").using("gin", t.effects)],
);

export const reports = pgTable(
  "reports",
  {
    presetId: integer("preset_id").notNull().references(() => presets.id, { onDelete: "cascade" }),
    userId: text("user_id").notNull().references(() => users.id),
    reason: text().notNull(),
    createdAt: timestamp("created_at", { withTimezone: true }).notNull().defaultNow(),
  },
  (t) => [primaryKey({ columns: [t.presetId, t.userId] })],
);
