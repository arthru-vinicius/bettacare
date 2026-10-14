ALTER TABLE "device_state" ADD COLUMN "wifi_bssid" text;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "wifi_channel" smallint;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "wifi_last_disconnect_reason" smallint;--> statement-breakpoint
ALTER TABLE "devices" ADD COLUMN "offline_alerted_at" timestamp with time zone;