-- 13: Player testing metrics (athletic testing: 40yd, vert, etc.)

create table if not exists public.player_testing (
  id          uuid primary key default extensions.uuid_generate_v4(),
  player_id   uuid not null references public.players(id) on delete cascade,
  metric_name text not null,
  value       numeric not null,
  unit        text not null,
  tested_at   timestamptz not null default now(),
  created_at  timestamptz not null default now(),

  unique (player_id, metric_name)
);

create index if not exists idx_player_testing_player_id on public.player_testing(player_id);

-- RLS
alter table public.player_testing enable row level security;

create policy "Coaches can CRUD testing for own team players"
  on public.player_testing for all
  using (
    exists (
      select 1 from public.players p
      join public.teams t on t.id = p.team_id
      where p.id = player_testing.player_id
        and t.coach_id = auth.uid()
    )
  );

create policy "Linked players can view own testing"
  on public.player_testing for select
  using (
    exists (
      select 1 from public.players p
      where p.id = player_testing.player_id
        and p.linked_user_id = auth.uid()
    )
  );
