-- 07: Workout templates

create table if not exists public.workout_templates (
  id          uuid primary key default extensions.uuid_generate_v4(),
  coach_id    uuid not null references auth.users(id) on delete cascade,
  sport       text not null default 'football',
  name        text not null,
  description text,
  content     jsonb not null default '{"version": 2, "exercises": []}'::jsonb,
  created_at  timestamptz not null default now()
);

create index if not exists idx_workout_templates_coach_id on public.workout_templates(coach_id);

-- RLS
alter table public.workout_templates enable row level security;

create policy "Coaches can CRUD own templates"
  on public.workout_templates for all
  using (auth.uid() = coach_id);

-- NOTE: "Linked players can view templates assigned to them" policy is created
-- in 08_workout_assignments.sql (after the workout_assignments table exists).
