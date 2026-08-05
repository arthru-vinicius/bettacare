-- Gerado por drizzle-kit e depois EDITADO À MÃO em dois pontos:
-- `telemetry` e `events` ganharam `PARTITION BY RANGE (received_at)`.
--
-- O drizzle-kit não exprime particionamento, e ele é indispensável: DELETE no
-- PostgreSQL não devolve espaço ao sistema de arquivos, então o teto de 1 GB
-- só é implementável derrubando partições inteiras. Ver a seção 5 de
-- docs/arquitetura-observabilidade.md.
--
-- A edição não gera drift: o drizzle-kit compara schema.ts com o snapshot em
-- meta/, nunca com o banco. Ao regenerar, reaplicar as duas cláusulas.
CREATE TYPE "public"."command_action" AS ENUM('light.set', 'fan.set_speed', 'fan.set_mode', 'config.apply', 'device.reboot');--> statement-breakpoint
CREATE TYPE "public"."command_status" AS ENUM('queued', 'sent', 'acked', 'rejected', 'expired', 'superseded');--> statement-breakpoint
CREATE TYPE "public"."command_target" AS ENUM('light', 'fan', 'config', 'device');--> statement-breakpoint
CREATE TYPE "public"."component" AS ENUM('system', 'wifi', 'api', 'rtc', 'temp', 'fan', 'light', 'button', 'pot', 'nvs', 'ota');--> statement-breakpoint
CREATE TYPE "public"."event_source" AS ENUM('device', 'server');--> statement-breakpoint
CREATE TYPE "public"."fan_mode" AS ENUM('auto', 'manual', 'manual_off', 'failsafe');--> statement-breakpoint
CREATE TYPE "public"."health_status" AS ENUM('ok', 'degraded', 'missing', 'fault', 'unknown');--> statement-breakpoint
CREATE TYPE "public"."light_source" AS ENUM('schedule', 'manual', 'button', 'command', 'boot');--> statement-breakpoint
CREATE TYPE "public"."severity" AS ENUM('debug', 'info', 'warn', 'error', 'fatal');--> statement-breakpoint
CREATE TABLE "commands" (
	"id" integer PRIMARY KEY GENERATED ALWAYS AS IDENTITY (sequence name "commands_id_seq" INCREMENT BY 1 MINVALUE 1 MAXVALUE 2147483647 START WITH 1 CACHE 1),
	"device_id" text NOT NULL,
	"target" "command_target" NOT NULL,
	"action" "command_action" NOT NULL,
	"payload" jsonb NOT NULL,
	"status" "command_status" DEFAULT 'queued' NOT NULL,
	"requested_by" text,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"sent_at" timestamp with time zone,
	"settled_at" timestamp with time zone,
	"error_code" text
);
--> statement-breakpoint
CREATE TABLE "component_status" (
	"device_id" text NOT NULL,
	"comp" "component" NOT NULL,
	"status" "health_status" DEFAULT 'unknown' NOT NULL,
	"since" timestamp with time zone DEFAULT now() NOT NULL,
	"last_ok_at" timestamp with time zone,
	"last_code" text,
	"detail" jsonb,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "component_status_device_id_comp_pk" PRIMARY KEY("device_id","comp")
);
--> statement-breakpoint
CREATE TABLE "device_state" (
	"device_id" text PRIMARY KEY NOT NULL,
	"light_on" boolean DEFAULT false NOT NULL,
	"light_source" "light_source" DEFAULT 'boot' NOT NULL,
	"temp_celsius" real,
	"temp_available" boolean DEFAULT false NOT NULL,
	"temp_valid" boolean DEFAULT false NOT NULL,
	"temp_age_ms" integer,
	"fan_on" boolean DEFAULT false NOT NULL,
	"fan_speed_percent" smallint DEFAULT 0 NOT NULL,
	"fan_rpm" integer DEFAULT 0 NOT NULL,
	"fan_mode" "fan_mode" DEFAULT 'auto' NOT NULL,
	"rtc_available" boolean DEFAULT false NOT NULL,
	"rtc_time" text,
	"rtc_lost_power" boolean DEFAULT false NOT NULL,
	"wifi_rssi" smallint,
	"wifi_ip" text,
	"wifi_reconnects" integer DEFAULT 0 NOT NULL,
	"uptime_ms" integer,
	"config_version" integer DEFAULT 0 NOT NULL,
	"last_boot_id" integer DEFAULT -1 NOT NULL,
	"last_seq" integer DEFAULT -1 NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	"device_time" timestamp with time zone
);
--> statement-breakpoint
CREATE TABLE "devices" (
	"device_id" text PRIMARY KEY NOT NULL,
	"name" text DEFAULT 'Aquário' NOT NULL,
	"fw_version" text,
	"last_seen_at" timestamp with time zone,
	"last_ip" text,
	"online" boolean DEFAULT false NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL
);
--> statement-breakpoint
CREATE TABLE "events" (
	"received_at" timestamp with time zone DEFAULT now() NOT NULL,
	"id" text DEFAULT gen_random_uuid() NOT NULL,
	"device_id" text NOT NULL,
	"source" "event_source" NOT NULL,
	"sev" "severity" NOT NULL,
	"comp" "component" NOT NULL,
	"code" text NOT NULL,
	"msg" text NOT NULL,
	"ctx" jsonb,
	"repeat_count" integer DEFAULT 1 NOT NULL,
	"device_time" timestamp with time zone,
	CONSTRAINT "events_received_at_id_pk" PRIMARY KEY("received_at","id")
) PARTITION BY RANGE ("received_at");
--> statement-breakpoint
CREATE TABLE "settings" (
	"device_id" text PRIMARY KEY NOT NULL,
	"config" jsonb NOT NULL,
	"config_version" integer DEFAULT 1 NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	"updated_by" text
);
--> statement-breakpoint
CREATE TABLE "telemetry" (
	"received_at" timestamp with time zone DEFAULT now() NOT NULL,
	"device_id" text NOT NULL,
	"boot_id" integer NOT NULL,
	"seq" integer NOT NULL,
	"device_time" timestamp with time zone,
	"light_on" boolean NOT NULL,
	"temp_celsius" real,
	"temp_valid" boolean DEFAULT false NOT NULL,
	"fan_on" boolean NOT NULL,
	"fan_speed_percent" smallint NOT NULL,
	"fan_rpm" integer NOT NULL,
	"fan_mode" "fan_mode" NOT NULL,
	"wifi_rssi" smallint,
	CONSTRAINT "telemetry_received_at_device_id_boot_id_seq_pk" PRIMARY KEY("received_at","device_id","boot_id","seq")
) PARTITION BY RANGE ("received_at");
--> statement-breakpoint
CREATE TABLE "telemetry_hourly" (
	"device_id" text NOT NULL,
	"hour" timestamp with time zone NOT NULL,
	"temp_min" real,
	"temp_avg" real,
	"temp_max" real,
	"light_minutes" smallint DEFAULT 0 NOT NULL,
	"fan_minutes" smallint DEFAULT 0 NOT NULL,
	"fan_rpm_avg" integer,
	"samples" integer DEFAULT 0 NOT NULL,
	CONSTRAINT "telemetry_hourly_device_id_hour_pk" PRIMARY KEY("device_id","hour")
);
--> statement-breakpoint
ALTER TABLE "commands" ADD CONSTRAINT "commands_device_id_devices_device_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("device_id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "component_status" ADD CONSTRAINT "component_status_device_id_devices_device_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("device_id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "device_state" ADD CONSTRAINT "device_state_device_id_devices_device_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("device_id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "settings" ADD CONSTRAINT "settings_device_id_devices_device_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("device_id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "telemetry_hourly" ADD CONSTRAINT "telemetry_hourly_device_id_devices_device_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("device_id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
CREATE INDEX "commands_pending_idx" ON "commands" USING btree ("device_id","created_at") WHERE "commands"."status" in ('queued', 'sent');--> statement-breakpoint
CREATE INDEX "commands_history_idx" ON "commands" USING btree ("device_id","created_at");--> statement-breakpoint
CREATE INDEX "events_filter_idx" ON "events" USING btree ("device_id","comp","sev","received_at");--> statement-breakpoint
CREATE INDEX "events_code_idx" ON "events" USING btree ("device_id","code","received_at");--> statement-breakpoint
CREATE INDEX "telemetry_received_at_brin" ON "telemetry" USING brin ("received_at") WITH (pages_per_range=32);