ALTER TYPE "public"."command_target" ADD VALUE 'feeder';--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_connected" boolean DEFAULT false NOT NULL;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_auto_enabled" boolean;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_hour1" smallint;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_hour2" smallint;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_grains_per_feeding" smallint;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_last_feed_at" timestamp with time zone;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_last_feed_requested" smallint;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_last_feed_confirmed" smallint;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "feeder_last_feed_ok" boolean;