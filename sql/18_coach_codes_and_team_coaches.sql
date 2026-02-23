-- 18: Coach codes + team_coaches junction table for multi-coach teams.
-- Run in Supabase SQL Editor.

-- 1a. Add coach_code to profiles (same pattern as player invite_code)
ALTER TABLE public.profiles
  ADD COLUMN IF NOT EXISTS coach_code text
    NOT NULL DEFAULT encode(gen_random_bytes(4), 'hex');
CREATE UNIQUE INDEX IF NOT EXISTS idx_profiles_coach_code
  ON public.profiles(coach_code);

-- 1b. Junction table for multi-coach teams
CREATE TABLE IF NOT EXISTS public.team_coaches (
  id        uuid PRIMARY KEY DEFAULT extensions.uuid_generate_v4(),
  team_id   uuid NOT NULL REFERENCES public.teams(id) ON DELETE CASCADE,
  coach_id  uuid NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  role      text NOT NULL DEFAULT 'assistant' CHECK (role IN ('head', 'assistant')),
  added_at  timestamptz NOT NULL DEFAULT now(),
  UNIQUE(team_id, coach_id)
);
CREATE INDEX IF NOT EXISTS idx_team_coaches_coach ON public.team_coaches(coach_id);

ALTER TABLE public.team_coaches ENABLE ROW LEVEL SECURITY;

-- 1c. Backfill existing team owners as head coaches
INSERT INTO public.team_coaches (team_id, coach_id, role)
SELECT id, coach_id, 'head' FROM public.teams
ON CONFLICT (team_id, coach_id) DO NOTHING;
