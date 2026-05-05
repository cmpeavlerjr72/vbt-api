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

    # 3. Tags matching uid on those teams
    try:
        tags_resp = (
            sb.table("rfid_tags")
            .select("assigned_player_id, team_id")
            .eq("uid", uid)
            .in_("team_id", team_ids)
            .execute()
        )
    except Exception as exc:
        log.exception("rfid tag lookup failed uid=%s", uid)
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
    sb = _require_db()

    try:
        # 1. Create raw set
        raw_set_resp = (
            sb.table("vbt_raw_sets")
            .insert({
                "player_id": body.player_id,
                "team_id": body.team_id,
                "exercise": body.exercise,
                "device_id": body.device_id,
                "samples": [],
                "processed": True,
            })
            .execute()
        )
        if not raw_set_resp or not raw_set_resp.data:
            raise HTTPException(status_code=500, detail="Failed to create raw set")
        raw_set = raw_set_resp.data[0]
        set_id = raw_set["id"]

        # 2. Create reps
        rep_rows = []
        for r in body.reps:
            rep_rows.append({
                "raw_set_id": set_id,
                "player_id": body.player_id,
                "exercise": body.exercise,
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
            })
        if rep_rows:
            sb.table("vbt_reps").insert(rep_rows).execute()

        # 3. Compute and create set summary
        mean_vels = [r.mean_velocity for r in body.reps]
        peak_vels = [r.peak_velocity for r in body.reps]
        avg_velocity = sum(mean_vels) / len(mean_vels) if mean_vels else 0
        peak_velocity = max(peak_vels) if peak_vels else 0

        velocity_loss = None
        if len(mean_vels) >= 2 and mean_vels[0] > 0:
            velocity_loss = (mean_vels[0] - mean_vels[-1]) / mean_vels[0] * 100

        sb.table("vbt_set_summaries").insert({
            "raw_set_id": set_id,
            "player_id": body.player_id,
            "exercise": body.exercise,
            "rep_count": len(body.reps),
            "avg_velocity": round(avg_velocity, 4),
            "peak_velocity": round(peak_velocity, 4),
            "velocity_loss": round(velocity_loss, 2) if velocity_loss is not None else None,
            "flagged": False,
        }).execute()

    except HTTPException:
        raise
    except Exception as exc:
        log.exception("device set creation failed")
        raise HTTPException(status_code=500, detail=f"Set creation failed: {exc}")

    return DeviceSetOut(set_id=set_id, reps_created=len(body.reps))
