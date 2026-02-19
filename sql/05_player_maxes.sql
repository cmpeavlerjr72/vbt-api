-- 05: Player maxes table

create table if not exists public.player_maxes (
  id          uuid primary key default extensions.uuid_generate_v4(),
  player_id   uuid not null references public.players(id) on delete cascade,
  exercise    text not null,
  weight      numeric not null,
  tested_at   timestamptz not null default now(),
  created_at  timestamptz not null default now(),

  unique (player_id, exercise)
);

create index if not exists idx_player_maxes_player_id on public.player_maxes(player_id);

-- RLS
alter table public.player_maxes enable row level security;

create policy "Coaches can CRUD maxes for own team players"
  on public.player_maxes for all
  using (
    exists (
      select 1 from public.players p
      join public.teams t on t.id = p.team_id
      where p.id = player_maxes.player_id
        and t.coach_id = auth.uid()
    )
  );

create policy "Linked players can view own maxes"
  on public.player_maxes for select
  using (
    exists (
      select 1 from public.players p
      where p.id = player_maxes.player_id
        and p.linked_user_id = auth.uid()
    )
  );
