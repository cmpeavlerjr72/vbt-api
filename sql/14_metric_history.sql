-- 14: History tables for tracking maxes and testing metrics over time.
-- The main tables (player_maxes, player_testing) keep the "current" value.
-- These history tables log every previous value before it's overwritten.

-- ── Max History ──────────────────────────────────────────────────────────────

create table if not exists public.player_max_history (
  id          uuid primary key default extensions.uuid_generate_v4(),
  player_id   uuid not null references public.players(id) on delete cascade,
  exercise    text not null,
  weight      numeric not null,
  tested_at   timestamptz not null,
  created_at  timestamptz not null default now()
);

create index if not exists idx_max_history_player
  on public.player_max_history(player_id, exercise, tested_at desc);

-- RLS
alter table public.player_max_history enable row level security;

create policy "Coaches can read max history for own team players"
  on public.player_max_history for select
  using (
    exists (
      select 1 from public.players p
      join public.teams t on t.id = p.team_id
      where p.id = player_max_history.player_id
        and t.coach_id = auth.uid()
    )
  );

create policy "Linked players can view own max history"
  on public.player_max_history for select
  using (
    exists (
      select 1 from public.players p
      where p.id = player_max_history.player_id
        and p.linked_user_id = auth.uid()
    )
  );

-- ── Testing History ─────────────────────────────────────────────────────────

create table if not exists public.player_testing_history (
  id          uuid primary key default extensions.uuid_generate_v4(),
  player_id   uuid not null references public.players(id) on delete cascade,
  metric_name text not null,
  value       numeric not null,
  unit        text not null,
  tested_at   timestamptz not null,
  created_at  timestamptz not null default now()
);

create index if not exists idx_testing_history_player
  on public.player_testing_history(player_id, metric_name, tested_at desc);

-- RLS
alter table public.player_testing_history enable row level security;

create policy "Coaches can read testing history for own team players"
  on public.player_testing_history for select
  using (
    exists (
      select 1 from public.players p
      join public.teams t on t.id = p.team_id
      where p.id = player_testing_history.player_id
        and t.coach_id = auth.uid()
    )
  );

create policy "Linked players can view own testing history"
  on public.player_testing_history for select
  using (
    exists (
      select 1 from public.players p
      where p.id = player_testing_history.player_id
        and p.linked_user_id = auth.uid()
    )
  );
