-- 21: Atomic /device/sets insert via PL/pgSQL function.
--
-- Replaces the old three-step pattern (insert raw_set, insert reps, insert
-- summary) which was vulnerable to partial-write failures if the network
-- blipped between steps. Now all three writes happen in a single Postgres
-- transaction — either everything commits or nothing does.
--
-- Returns: jsonb { set_id: uuid, reps_created: int }

CREATE OR REPLACE FUNCTION public.insert_device_set(
  p_player_id      uuid,
  p_team_id        uuid,
  p_exercise       text,
  p_device_id      text,
  p_reps           jsonb,
  p_avg_velocity   numeric,
  p_peak_velocity  numeric,
  p_velocity_loss  numeric
) RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_set_id    uuid;
  v_rep_count int;
BEGIN
  -- 1. Raw set row
  INSERT INTO public.vbt_raw_sets (
    player_id, team_id, exercise, device_id, samples, processed
  )
  VALUES (
    p_player_id, p_team_id, p_exercise, p_device_id, '[]'::jsonb, true
  )
  RETURNING id INTO v_set_id;

  -- 2. Rep rows (one per element in p_reps array)
  INSERT INTO public.vbt_reps (
    raw_set_id, player_id, exercise, rep_number,
    mean_velocity, peak_velocity, rom_meters,
    concentric_duration, eccentric_duration,
    conc_peak_accel, ecc_peak_velocity, ecc_peak_accel,
    samples
  )
  SELECT
    v_set_id,
    p_player_id,
    p_exercise,
    (r->>'rep_number')::int,
    (r->>'mean_velocity')::numeric,
    (r->>'peak_velocity')::numeric,
    NULLIF(r->>'rom_meters', '')::numeric,
    NULLIF(r->>'concentric_duration', '')::numeric,
    NULLIF(r->>'eccentric_duration', '')::numeric,
    NULLIF(r->>'conc_peak_accel', '')::numeric,
    NULLIF(r->>'ecc_peak_velocity', '')::numeric,
    NULLIF(r->>'ecc_peak_accel', '')::numeric,
    COALESCE(r->'samples', '[]'::jsonb)
  FROM jsonb_array_elements(p_reps) AS r;

  GET DIAGNOSTICS v_rep_count = ROW_COUNT;

  -- 3. Summary row
  INSERT INTO public.vbt_set_summaries (
    raw_set_id, player_id, exercise, rep_count,
    avg_velocity, peak_velocity, velocity_loss, flagged
  )
  VALUES (
    v_set_id, p_player_id, p_exercise, v_rep_count,
    p_avg_velocity, p_peak_velocity, p_velocity_loss, false
  );

  RETURN jsonb_build_object('set_id', v_set_id, 'reps_created', v_rep_count);
END;
$$;

-- The endpoint runs with the service role; grant execute so the RPC works
-- when called via PostgREST's /rpc path.
GRANT EXECUTE ON FUNCTION public.insert_device_set(
  uuid, uuid, text, text, jsonb, numeric, numeric, numeric
) TO service_role;
