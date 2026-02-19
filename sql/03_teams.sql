-- 03: Teams table

create table if not exists public.teams (
  id              uuid primary key default extensions.uuid_generate_v4(),
  coach_id        uuid not null references auth.users(id) on delete cascade,
  name            text not null,
  sport           text not null default 'football',
  dashboard_config jsonb not null default '{}'::jsonb,
  created_at      timestamptz not null default now()
);

create index if not exists idx_teams_coach_id on public.teams(coach_id);

-- RLS
alter table public.teams enable row level security;

create policy "Coaches can CRUD own teams"
  on public.teams for all
  using (auth.uid() = coach_id);

-- NOTE: "Linked players can view their team" policy is created in 04_players.sql
-- (after the players table exists).
