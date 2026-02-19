-- 04: Players table

create table if not exists public.players (
  id              uuid primary key default extensions.uuid_generate_v4(),
  team_id         uuid not null references public.teams(id) on delete cascade,
  first_name      text not null default '',
  last_name       text not null default '',
  jersey_number   int,
  position_group  text not null default 'skill'
                  check (position_group in ('skill', 'combo', 'power')),
  rfid_tag_id     uuid,
  invite_code     text not null default encode(gen_random_bytes(4), 'hex'),
  linked_user_id  uuid references auth.users(id) on delete set null,
  linked_at       timestamptz,
  created_at      timestamptz not null default now()
);

create index if not exists idx_players_team_id on public.players(team_id);
create unique index if not exists idx_players_invite_code on public.players(invite_code);
create index if not exists idx_players_linked_user_id on public.players(linked_user_id);

-- RLS
alter table public.players enable row level security;

create policy "Coaches can CRUD players on own teams"
  on public.players for all
  using (
    exists (
      select 1 from public.teams
      where teams.id = players.team_id
        and teams.coach_id = auth.uid()
    )
  );

create policy "Linked players can view own row"
  on public.players for select
  using (linked_user_id = auth.uid());

create policy "Players can view teammates"
  on public.players for select
  using (
    exists (
      select 1 from public.players as me
      where me.linked_user_id = auth.uid()
        and me.team_id = players.team_id
    )
  );

-- Deferred policy from 03_teams.sql (needs players table to exist)
create policy "Linked players can view their team"
  on public.teams for select
  using (
    exists (
      select 1 from public.players
      where players.team_id = teams.id
        and players.linked_user_id = auth.uid()
    )
  );
