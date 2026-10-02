ALTER TABLE "device_state" ADD COLUMN "reset_reason" text;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "free_heap" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "min_free_heap" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "max_alloc_heap" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "post_latency_ms" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "api_failures" integer DEFAULT 0 NOT NULL;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "pot_raw_adc" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "button_pressed" boolean DEFAULT false NOT NULL;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "tach_pulses_raw" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "net_task_stack_hwm" integer;--> statement-breakpoint
ALTER TABLE "device_state" ADD COLUMN "boot_count" integer;--> statement-breakpoint
ALTER TABLE "telemetry" ADD COLUMN "temp_age_ms" bigint;--> statement-breakpoint
ALTER TABLE "telemetry" ADD COLUMN "rtc_available" boolean;--> statement-breakpoint
ALTER TABLE "telemetry" ADD COLUMN "rtc_lost_power" boolean;--> statement-breakpoint
ALTER TABLE "telemetry" ADD COLUMN "wifi_reconnects" integer;--> statement-breakpoint
ALTER TABLE "telemetry" ADD COLUMN "free_heap" integer;--> statement-breakpoint
ALTER TABLE "telemetry" ADD COLUMN "max_alloc_heap" integer;--> statement-breakpoint
ALTER TABLE "telemetry" ADD COLUMN "post_latency_ms" integer;--> statement-breakpoint
ALTER TABLE "telemetry" ADD COLUMN "pot_raw_adc" integer;