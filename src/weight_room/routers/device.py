"""Device endpoints for ESP32 VBT units (open for v1 testing)."""
from __future__ import annotations

import logging
from typing import List

from fastapi import APIRouter, HTTPException

from weight_room.core.models import DevicePlayerOut, DeviceSetIn, DeviceSetOut
from weight_room.db import get_supabase

log = logging.getLogger(__name__)

router = APIRouter(prefix="/device", tags=["device"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


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
