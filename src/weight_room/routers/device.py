"""Device endpoints for ESP32 VBT units (open for v1 testing)."""
from __future__ import annotations

import logging
from typing import List

from fastapi import APIRouter, HTTPException, Query

from weight_room.core.models import (
    DeviceLookupPlayer,
    DevicePlayerOut,
    DeviceSetIn,
    DeviceSetOut,
    DeviceStatusOut,
)
from weight_room.db import get_supabase


def _uid_candidates(uid: str) -> list[str]:
    """Return all plausible string forms of a scanned UID so we can match
    tags registered via different readers (hex vs decimal, big- vs little-endian).
    """
    raw = uid.strip()
    forms: set[str] = {raw}

    hex_clean = raw.replace(":", "").replace("-", "").replace(" ", "")
    if hex_clean:
        try:
            byts = bytes.fromhex(hex_clean)
            be_int = int.from_bytes(byts, "big")
            le_int = int.from_bytes(byts, "little")
            forms.update({
                hex_clean.upper(),
                hex_clean.lower(),
                str(be_int),
                str(le_int),
                str(be_int).zfill(10),  # 10-digit decimal printed on EM cards
                str(le_int).zfill(10),
            })
        except ValueError:
            pass  # not hex — likely already decimal, treat raw as the only form

    if raw.isdigit():
        try:
            n = int(raw)
            byts = n.to_bytes(4, "big")
            forms.update({
                byts.hex().upper(),
                ":".join(f"{b:02X}" for b in byts),
                ":".join(f"{b:02X}" for b in reversed(byts)),
                bytes(reversed(byts)).hex().upper(),
            })
        except (ValueError, OverflowError):
            pass

    return list(forms)

log = logging.getLogger(__name__)

router = APIRouter(prefix="/device", tags=["device"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("/status", response_model=DeviceStatusOut)
def device_status(device_id: str = Query(...)):
    """Tell the device whether it's been paired to a coach yet."""
    sb = _require_db()
    try:
        resp = (
            sb.table("devices")
            .select("coach_id")
            .eq("device_id", device_id)
            .maybe_single()
            .execute()
        )
    except Exception as exc:
        log.exception("device status lookup failed for device=%s", device_id)
        raise HTTPException(status_code=500, detail=f"Status lookup failed: {exc}")

    if not resp or not resp.data:
        return DeviceStatusOut(paired=False)

    coach_id = resp.data.get("coach_id")
    if not coach_id:
        # Provisioned placeholder row but no coach has claimed it yet.
        return DeviceStatusOut(paired=False)

    coach_name = None
    try:
        prof_resp = (
            sb.table("profiles")
            .select("display_name")
            .eq("id", coach_id)
            .maybe_single()
            .execute()
        )
        if prof_resp and prof_resp.data:
            coach_name = prof_resp.data.get("display_name")
    except Exception:
        log.exception("coach name lookup failed for coach=%s", coach_id)

    return DeviceStatusOut(paired=True, coach_name=coach_name)


@router.get("/lookup", response_model=List[DeviceLookupPlayer])
def lookup_tag(device_id: str = Query(...), uid: str = Query(...)):
    """Match a scanned RFID UID to player(s) on any team the paired coach can access.

    Returns 0, 1, or N matches. The device picks (or asks the coach to pick) one.
    """
    sb = _require_db()

    # 1. Resolve device → coach
    try:
        dev_resp = (
            sb.table("devices")
            .select("coach_id")
            .eq("device_id", device_id)
            .maybe_single()
            .execute()
        )
    except Exception as exc:
        log.exception("device lookup failed device=%s", device_id)
        raise HTTPException(status_code=500, detail=f"Device lookup failed: {exc}")

    if not dev_resp or not dev_resp.data:
        raise HTTPException(status_code=404, detail="Device not paired")
    coach_id = dev_resp.data["coach_id"]

    # 2. Coach's accessible teams (head + assistant via team_coaches junction)
    try:
        teams_resp = (
            sb.table("team_coaches")
            .select("team_id")
            .eq("coach_id", coach_id)
            .execute()
        )
    except Exception as exc:
        log.exception("team_coaches fetch failed coach=%s", coach_id)
        raise HTTPException(status_code=500, detail=f"Team fetch failed: {exc}")

    team_ids = [t["team_id"] for t in (teams_resp.data or [])]
    if not team_ids:
        return []

    # 3. Tags matching uid on those teams. Different readers print the same
    # card in different formats (hex vs decimal, BE vs LE byte order), so we
    # match against every plausible string form of the incoming UID.
    uid_forms = _uid_candidates(uid)
    try:
        tags_resp = (
            sb.table("rfid_tags")
            .select("assigned_player_id, team_id")
            .in_("uid", uid_forms)
            .in_("team_id", team_ids)
            .execute()
        )
    except Exception as exc:
        log.exception("rfid tag lookup failed uid=%s forms=%s", uid, uid_forms)
        raise HTTPException(status_code=500, detail=f"Tag lookup failed: {exc}")

    tag_rows = [t for t in (tags_resp.data or []) if t.get("assigned_player_id")]
    if not tag_rows:
        return []

    player_ids = [t["assigned_player_id"] for t in tag_rows]

    # 4. Player + team info
    try:
        p_resp = (
            sb.table("players")
            .select("id, first_name, last_name, jersey_number, team_id")
            .in_("id", player_ids)
            .execute()
        )
    except Exception as exc:
        log.exception("player fetch failed for tag uid=%s", uid)
        raise HTTPException(status_code=500, detail=f"Player fetch failed: {exc}")

    players = p_resp.data or []
    if not players:
        return []

    # Best-effort team name lookup (small set)
    team_names: dict[str, str] = {}
    try:
        t_resp = (
            sb.table("teams")
            .select("id, name")
            .in_("id", list({p["team_id"] for p in players}))
            .execute()
        )
        for row in t_resp.data or []:
            team_names[row["id"]] = row["name"]
    except Exception:
        log.exception("team name fetch failed")

    return [
        DeviceLookupPlayer(
            id=p["id"],
            first_name=p["first_name"],
            last_name=p["last_name"],
            jersey_number=p.get("jersey_number"),
            team_id=p["team_id"],
            team_name=team_names.get(p["team_id"]),
        )
        for p in players
    ]


@router.get("/exercises", response_model=List[str])
def device_exercises(device_id: str = Query(...), player_id: str = Query(...)):
    """List the player's assigned exercises across all active workout assignments.

    The device calls this after an RFID scan to populate its exercise picker.
    Returns a flat list of exercise names. Empty list = no active assignments
    (the device falls back to its built-in default list).
    """
    sb = _require_db()

    # 1. Verify device is paired and get coach.
    dev_resp = (
        sb.table("devices")
        .select("coach_id")
        .eq("device_id", device_id)
        .maybe_single()
        .execute()
    )
    if not dev_resp or not dev_resp.data:
        raise HTTPException(status_code=404, detail="Device not paired")
    coach_id = dev_resp.data["coach_id"]

    # 2. Player + team membership; team must be one the coach can access.
    player_resp = (
        sb.table("players")
        .select("id, team_id, position_group")
        .eq("id", player_id)
        .maybe_single()
        .execute()
    )
    if not player_resp or not player_resp.data:
        raise HTTPException(status_code=404, detail="Player not found")
    player = player_resp.data
    team_id = player["team_id"]
    position_group = player.get("position_group")

    access_resp = (
        sb.table("team_coaches")
        .select("team_id")
        .eq("coach_id", coach_id)
        .eq("team_id", team_id)
        .execute()
    )
    if not access_resp or not access_resp.data:
        raise HTTPException(status_code=403, detail="Player not on this device's team")

    # 3. Active assignments for the team.
    assigns = (
        sb.table("workout_assignments")
        .select("id, template_id, target_type, target_position_group")
        .eq("team_id", team_id)
        .eq("status", "active")
        .execute()
    )

    # Lazy-import the template parser to avoid cross-router import cycles at startup.
    from weight_room.routers.workouts import _parse_exercises

    seen: set[str] = set()
    ordered: list[str] = []
    for a in (assigns.data or []):
        target = a.get("target_type", "team")
        if target == "position_group":
            if a.get("target_position_group") != position_group:
                continue
        elif target == "players":
            j = (
                sb.table("workout_assignment_players")
                .select("player_id")
                .eq("assignment_id", a["id"])
                .eq("player_id", player_id)
                .execute()
            )
            if not j.data:
                continue

        tmpl = (
            sb.table("workout_templates")
            .select("content")
            .eq("id", a["template_id"])
            .maybe_single()
            .execute()
        )
        if not tmpl or not tmpl.data:
            continue

        for ex in _parse_exercises(tmpl.data.get("content", {})):
            name = (ex.get("exercise_name") or "").strip()
            if name and name not in seen:
                seen.add(name)
                ordered.append(name)

    return ordered


@router.get("/roster/{team_id}", response_model=List[DevicePlayerOut])
def get_roster(team_id: str):
    sb = _require_db()
    try:
        resp = (
            sb.table("players")
            .select("id, first_name, last_name, jersey_number")
            .eq("team_id", team_id)
            .order("jersey_number", desc=False)
            .execute()
        )
    except Exception as exc:
        log.exception("roster fetch failed for team=%s", team_id)
        raise HTTPException(status_code=500, detail=f"Roster fetch failed: {exc}")
    return resp.data or []


@router.post("/sets", response_model=DeviceSetOut, status_code=201)
def create_set(body: DeviceSetIn):
    """Persist a completed set (raw_set + reps + summary) atomically.

    All three table writes happen inside a single Postgres transaction via
    the insert_device_set RPC (sql/21_insert_device_set_rpc.sql). Either
    everything commits or nothing does — no more orphaned raw_sets if the
    network blips between writes.
    """
    sb = _require_db()

    # Compute set-level summary stats in Python so the SQL function stays dumb.
    mean_vels = [r.mean_velocity for r in body.reps]
    peak_vels = [r.peak_velocity for r in body.reps]
    avg_velocity = sum(mean_vels) / len(mean_vels) if mean_vels else 0
    peak_velocity = max(peak_vels) if peak_vels else 0
    velocity_loss = None
    if len(mean_vels) >= 2 and mean_vels[0] > 0:
        velocity_loss = (mean_vels[0] - mean_vels[-1]) / mean_vels[0] * 100

    # Serialize reps as plain JSON for the RPC's jsonb argument.
    reps_payload = [
        {
            "rep_number": r.rep_number,
            "mean_velocity": r.mean_velocity,
            "peak_velocity": r.peak_velocity,
            "rom_meters": r.rom_meters,
            "concentric_duration": r.concentric_duration,
            "eccentric_duration": r.eccentric_duration,
            "conc_peak_accel": r.conc_peak_accel,
            "ecc_peak_velocity": r.ecc_peak_velocity,
            "ecc_peak_accel": r.ecc_peak_accel,
            "samples": [s.model_dump() for s in r.samples],
        }
        for r in body.reps
    ]

    try:
        resp = sb.rpc(
            "insert_device_set",
            {
                "p_player_id":     body.player_id,
                "p_team_id":       body.team_id,
                "p_exercise":      body.exercise,
                "p_device_id":     body.device_id,
                "p_reps":          reps_payload,
                "p_avg_velocity":  round(avg_velocity, 4),
                "p_peak_velocity": round(peak_velocity, 4),
                "p_velocity_loss": round(velocity_loss, 2) if velocity_loss is not None else None,
            },
        ).execute()
    except Exception as exc:
        log.exception("device set creation failed")
        raise HTTPException(status_code=500, detail=f"Set creation failed: {exc}")

    if not resp or not resp.data:
        raise HTTPException(status_code=500, detail="insert_device_set returned no data")

    # The RPC returns jsonb {"set_id": uuid, "reps_created": int}; PostgREST
    # delivers it either wrapped in a list (one row) or directly as the dict.
    payload = resp.data[0] if isinstance(resp.data, list) else resp.data
    return DeviceSetOut(
        set_id=payload["set_id"],
        reps_created=payload["reps_created"],
    )
