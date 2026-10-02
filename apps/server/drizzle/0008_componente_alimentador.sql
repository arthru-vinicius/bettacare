ALTER TYPE "public"."command_action" ADD VALUE 'feeder.feed_now';--> statement-breakpoint
ALTER TYPE "public"."command_action" ADD VALUE 'feeder.set_config';--> statement-breakpoint
ALTER TYPE "public"."component" ADD VALUE 'feeder' BEFORE 'nvs';