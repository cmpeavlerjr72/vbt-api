-- 19: Devices table — pairs an ESP32 VBT unit to a coach.
-- The device's hardware-derived device_id (e.g. "vbt-A4B2C9") is unique.
-- A scan from the device looks up coach_id, then searches all teams that
-- coach can access (via team_coaches junction) for a matching rfid_tag.

create table if not exists public.devices (
  id          uuid primary key default extensions.uuid_generate_v4(),
  device_id   text unique not null,
  coach_id    uuid not null references auth.users(id) on delete cascade,
  label       text,
  created_at  timestamptz not null default now()
);

create index if not exists idx_devices_coach_id on public.devices(coach_id);

alter table public.devices enable row level security;

create policy "Coaches can CRUD own devices"
  on public.devices for all
  using (auth.uid() = coach_id);
