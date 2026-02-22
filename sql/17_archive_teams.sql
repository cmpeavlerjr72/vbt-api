ALTER TABLE public.teams
  ADD COLUMN IF NOT EXISTS archived boolean NOT NULL DEFAULT false;
