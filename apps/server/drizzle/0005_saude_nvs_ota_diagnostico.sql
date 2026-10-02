ALTER TYPE "public"."command_action" ADD VALUE 'device.diagnose';--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "nvs_failures" integer DEFAULT 0 NOT NULL;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "ota_last_result" text DEFAULT 'none' NOT NULL;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "events_dropped" integer DEFAULT 0 NOT NULL;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "diagnostic_ran_at" timestamp with time zone;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "diagnostic_command_id" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "diagnostic_duration_ms" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "diagnostic_overall" "health_status";--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "diagnostic_checks" jsonb;