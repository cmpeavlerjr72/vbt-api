-- 10: RPC functions

-- claim_invite_code: Links a user to a player via invite code.
-- Accepts uid explicitly so it works when called with the service-role key
-- (auth.uid() is NULL for service-role calls).
-- Returns the player row on success, raises an exception otherwise.
create or replace function public.claim_invite_code(code text, uid uuid default null)
returns json
language plpgsql
security definer set search_path = ''
as $$
declare
  v_player_id uuid;
  v_team_id uuid;
  v_linked_user_id uuid;
  v_uid uuid;
  v_result json;
begin
  -- Prefer explicit uid param, fall back to auth.uid() for direct client calls
  v_uid := coalesce(uid, auth.uid());
  if v_uid is null then
    raise exception 'No user id provided';
  end if;

  -- Find the player with this invite code
  select id, team_id, linked_user_id
  into v_player_id, v_team_id, v_linked_user_id
  from public.players
  where invite_code = code;

  if v_player_id is null then
    raise exception 'Invalid invite code';
  end if;

  if v_linked_user_id is not null then
    raise exception 'This invite code has already been claimed';
  end if;

  -- Check the calling user isn't already linked to another player
  if exists (
    select 1 from public.players
    where linked_user_id = v_uid
  ) then
    raise exception 'You are already linked to a player';
  end if;

  -- Link the user
  update public.players
  set linked_user_id = v_uid,
      linked_at = now()
  where id = v_player_id;

  -- Return the updated player as JSON
  select row_to_json(p) into v_result
  from public.players p
  where p.id = v_player_id;

  return v_result;
end;
$$;
