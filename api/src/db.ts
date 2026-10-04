import type { PgDatabase, PgQueryResultHKT } from "drizzle-orm/pg-core";

// Postgres in production, PGlite in the tests.
export type Db = PgDatabase<PgQueryResultHKT>;
