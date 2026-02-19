-- 06: RFID tags + scan events

create table if not exists public.rfid_tags (
  id                  uuid primary key default extensions.uuid_generate_v4(),
  uid                 text not null,
  team_id             uuid not null references public.teams(id) on delete cascade,
  assigned_player_id  uuid references public.players(id) on delete set null,

  unique (uid, team_id)
);

create index if not exists idx_rfid_tags_team_id on public.rfid_tags(team_id);

create table if not exists public.scan_events (
  id          uuid primary key default extensions.uuid_generate_v4(),
  team_id     uuid not null references public.teams(id) on delete cascade,
  uid         text not null,
  device_id   text,
  created_at  timestamptz not null default now()
);

create index if not exists idx_scan_events_team_id on public.scan_events(team_id);

-- Enable realtime for scan_events
alter publication supabase_realtime add table public.scan_events;

-- RLS
alter table public.rfid_tags enable row level security;
alter table public.scan_events enable row level security;

create policy "Coaches can CRUD rfid_tags on own teams"
  on public.rfid_tags for all
  using (
    exists (
      select 1 from public.teams
      where teams.id = rfid_tags.team_id
        and teams.coach_id = auth.uid()
    )
  );

create policy "Coaches can CRUD scan_events on own teams"
  on public.scan_events for all
  using (
    exists (
      select 1 from public.teams
      where teams.id = scan_events.team_id
        and teams.coach_id = auth.uid()
    )
  );

create policy "Linked players can view scan_events on own team"
  on public.scan_events for select
  using (
    exists (
      select 1 from public.players
      where players.team_id = scan_events.team_id
        and players.linked_user_id = auth.uid()
    )
  );
