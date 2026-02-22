-- 15: Add tracked_columns to teams for roster column preferences

ALTER TABLE public.teams
  ADD COLUMN IF NOT EXISTS tracked_columns jsonb NOT NULL DEFAULT '[]'::jsonb;
