ALTER TABLE "device_state" ADD COLUMN "light_desired" boolean;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "last_history_at" timestamp with time zone;