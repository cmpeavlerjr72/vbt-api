-- 12: Add per-phase peak metrics (concentric/eccentric accel + velocity)

alter table public.vbt_reps
  add column if not exists conc_peak_accel numeric,
  add column if not exists ecc_peak_velocity numeric,
  add column if not exists ecc_peak_accel numeric;
