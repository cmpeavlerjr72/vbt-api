-- 08: Workout assignments + player junction

create table if not exists public.workout_assignments (
  id                      uuid primary key default extensions.uuid_generate_v4(),
  team_id                 uuid not null references public.teams(id) on delete cascade,
  template_id             uuid not null references public.workout_templates(id) on delete cascade,
  target_type             text not null default 'team'
                          check (target_type in ('team', 'position_group', 'players')),
  target_position_group   text
                          check (target_position_group is null or target_position_group in ('skill', 'combo', 'power')),
  start_at                timestamptz,
  due_at                  timestamptz,
  status                  text not null default 'active',
  notes                   text,
  created_by              uuid references auth.users(id) on delete set null,
  created_at              timestamptz not null default now()
);

create index if not exists idx_workout_assignments_team_id on public.workout_assignments(team_id);
create index if not exists idx_workout_assignments_template_id on public.workout_assignments(template_id);

-- Junction table for player-specific assignments
create table if not exists public.workout_assignment_players (
  id              uuid primary key default extensions.uuid_generate_v4(),
  assignment_id   uuid not null references public.workout_assignments(id) on delete cascade,
  player_id       uuid not null references public.players(id) on delete cascade,

  unique (assignment_id, player_id)
);

create index if not exists idx_wap_assignment_id on public.workout_assignment_players(assignment_id);
create index if not exists idx_wap_player_id on public.workout_assignment_players(player_id);

-- RLS
alter table public.workout_assignments enable row level security;
alter table public.workout_assignment_players enable row level security;

create policy "Coaches can CRUD assignments on own teams"
  on public.workout_assignments for all
  using (
    exists (
      select 1 from public.teams
      where teams.id = workout_assignments.team_id
        and teams.coach_id = auth.uid()
    )
  );

create policy "Linked players can view assignments on own team"
  on public.workout_assignments for select
  using (
    exists (
      select 1 from public.players
      where players.team_id = workout_assignments.team_id
        and players.linked_user_id = auth.uid()
    )
  );

create policy "Coaches can CRUD assignment_players on own teams"
  on public.workout_assignment_players for all
  using (
    exists (
      select 1 from public.workout_assignments wa
      join public.teams t on t.id = wa.team_id
      where wa.id = workout_assignment_players.assignment_id
        and t.coach_id = auth.uid()
    )
  );

create policy "Linked players can view own assignment_players"
  on public.workout_assignment_players for select
  using (
    player_id in (
      select p.id from public.players p
      where p.linked_user_id = auth.uid()
    )
  );

-- Deferred policy from 07_workout_templates.sql (needs workout_assignments table to exist)
create policy "Linked players can view templates assigned to them"
  on public.workout_templates for select
  using (
    exists (
      select 1 from public.workout_assignments wa
      join public.players p on p.team_id = wa.team_id
      where wa.template_id = workout_templates.id
        and p.linked_user_id = auth.uid()
    )
  );
