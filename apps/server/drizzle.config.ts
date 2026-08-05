import { defineConfig } from "drizzle-kit";

export default defineConfig({
  dialect: "postgresql",
  schema: "./src/db/schema.ts",
  out: "./drizzle",
  dbCredentials: {
    url:
      process.env.DATABASE_URL ??
      "postgresql://bettacare_user:bettacare_dev@127.0.0.1:5433/bettacare",
  },
  // As migrations são aplicadas na subida do processo, por
  // `drizzle-orm/node-postgres/migrator`. Este arquivo serve ao `generate`.
  verbose: true,
  strict: true,
});
