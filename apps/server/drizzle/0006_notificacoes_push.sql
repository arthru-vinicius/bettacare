CREATE TABLE "push_subscriptions" (
	"endpoint" text PRIMARY KEY NOT NULL,
	"p256dh" text NOT NULL,
	"auth" text NOT NULL,
	"created_by" text,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"failed_at" timestamp with time zone,
	"failure_count" integer DEFAULT 0 NOT NULL
);
--> statement-breakpoint
CREATE TABLE "server_keys" (
	"name" text PRIMARY KEY NOT NULL,
	"value" text NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL
);
