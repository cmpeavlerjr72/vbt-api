-- 09: VBT data tables (raw sets, reps, set summaries)

create table if not exists public.vbt_raw_sets (
  id          uuid primary key default extensions.uuid_generate_v4(),
  player_id   uuid not null references public.players(id) on delete cascade,
  team_id     uuid not null references public.teams(id) on delete cascade,
  exercise    text not null,
  device_id   text,
  sample_rate numeric,
  samples     jsonb not null default '[]'::jsonb,
  started_at  timestamptz not null default now(),
  ended_at    timestamptz not null default now(),
  processed   boolean not null default false,
  created_at  timestamptz not null default now()
);

create index if not exists idx_vbt_raw_sets_player_id on public.vbt_raw_sets(player_id);
create index if not exists idx_vbt_raw_sets_team_id on public.vbt_raw_sets(team_id);

create table if not exists public.vbt_reps (
  id                    uuid primary key default extensions.uuid_generate_v4(),
  raw_set_id            uuid not null references public.vbt_raw_sets(id) on delete cascade,
  player_id             uuid not null references public.players(id) on delete cascade,
  exercise              text not null,
  rep_number            int not null,
  mean_velocity         numeric not null,
  peak_velocity         numeric not null,
  eccentric_duration    numeric,
  concentric_duration   numeric,
  rom_meters            numeric,
  time_to_peak_vel      numeric,
  velocity_loss_pct     numeric,
  bar_path_deviation    numeric,
  flagged               boolean not null default false,
  flag_reason           text,
  created_at            timestamptz not null default now()
);

create index if not exists idx_vbt_reps_raw_set_id on public.vbt_reps(raw_set_id);
create index if not exists idx_vbt_reps_player_id on public.vbt_reps(player_id);

create table if not exists public.vbt_set_summaries (
  id              uuid primary key default extensions.uuid_generate_v4(),
  raw_set_id      uuid not null references public.vbt_raw_sets(id) on delete cascade,
  player_id       uuid not null references public.players(id) on delete cascade,
  exercise        text not null,
  rep_count       int not null,
  avg_velocity    numeric not null,
  peak_velocity   numeric not null,
  velocity_loss   numeric,
  estimated_1rm   numeric,
  flagged         boolean not null default false,
  flag_reason     text,
  created_at      timestamptz not null default now()
);

create index if not exists idx_vbt_set_summaries_raw_set_id on public.vbt_set_summaries(raw_set_id);
create index if not exists idx_vbt_set_summaries_player_id on public.vbt_set_summaries(player_id);

-- RLS
alter table public.vbt_raw_sets enable row level security;
alter table public.vbt_reps enable row level security;
alter table public.vbt_set_summaries enable row level security;

-- Coaches
create policy "Coaches can CRUD vbt_raw_sets on own teams"
  on public.vbt_raw_sets for all
  using (
    exists (
      select 1 from public.teams
      where teams.id = vbt_raw_sets.team_id
        and teams.coach_id = auth.uid()
    )
  );

create policy "Coaches can CRUD vbt_reps on own teams"
  on public.vbt_reps for all
  using (
    exists (
      select 1 from public.vbt_raw_sets rs
      join public.teams t on t.id = rs.team_id
      where rs.id = vbt_reps.raw_set_id
        and t.coach_id = auth.uid()
    )
  );

create policy "Coaches can CRUD vbt_set_summaries on own teams"
  on public.vbt_set_summaries for all
  using (
    exists (
      select 1 from public.vbt_raw_sets rs
      join public.teams t on t.id = rs.team_id
      where rs.id = vbt_set_summaries.raw_set_id
        and t.coach_id = auth.uid()
    )
  );

-- Players
create policy "Players can view own vbt_raw_sets"
  on public.vbt_raw_sets for select
  using (
    exists (
      select 1 from public.players p
      where p.id = vbt_raw_sets.player_id
        and p.linked_user_id = auth.uid()
    )
  );

create policy "Players can view own vbt_reps"
  on public.vbt_reps for select
  using (
    exists (
      select 1 from public.players p
      where p.id = vbt_reps.player_id
        and p.linked_user_id = auth.uid()
    )
  );

create policy "Players can view own vbt_set_summaries"
  on public.vbt_set_summaries for select
  using (
    exists (
      select 1 from public.players p
      where p.id = vbt_set_summaries.player_id
        and p.linked_user_id = auth.uid()
    )
  );
