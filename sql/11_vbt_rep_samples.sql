-- 11: Add per-rep velocity samples (20Hz curve data from ESP32)

alter table public.vbt_reps
  add column if not exists samples jsonb not null default '[]'::jsonb;
