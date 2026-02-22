"""Player testing endpoints (athletic metrics: 40yd, vert, etc.)."""
from __future__ import annotations

from datetime import datetime, timezone
from typing import List

from fastapi import APIRouter, Depends, HTTPException, Query

from weight_room.auth import get_current_user
from weight_room.core.models import PlayerTestingHistoryOut, PlayerTestingOut, PlayerTestingUpsert
from weight_room.db import get_supabase

router = APIRouter(tags=["testing"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("/players/{player_id}/testing", response_model=List[PlayerTestingOut])
def list_player_testing(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("player_testing")
        .select("*")
        .eq("player_id", player_id)
        .order("metric_name")
        .execute()
    )
    return resp.data


@router.post("/players/{player_id}/testing", response_model=PlayerTestingOut)
def upsert_player_testing(
    player_id: str, body: PlayerTestingUpsert, user_id: str = Depends(get_current_user)
):
    sb = _require_db()

    # Archive the existing value to history before overwriting
    existing = (
        sb.table("player_testing")
        .select("*")
        .eq("player_id", player_id)
        .eq("metric_name", body.metric_name)
        .maybe_single()
        .execute()
    )
    if existing.data:
        sb.table("player_testing_history").insert({
            "player_id": existing.data["player_id"],
            "metric_name": existing.data["metric_name"],
            "value": existing.data["value"],
            "unit": existing.data["unit"],
            "tested_at": existing.data["tested_at"],
        }).execute()

    # Upsert the new current value
    resp = (
        sb.table("player_testing")
        .upsert(
            {
                "player_id": player_id,
                "metric_name": body.metric_name,
                "value": body.value,
                "unit": body.unit,
                "tested_at": datetime.now(timezone.utc).isoformat(),
            },
            on_conflict="player_id,metric_name",
        )
        .execute()
    )
    return resp.data[0]


@router.get("/players/{player_id}/testing/history", response_model=List[PlayerTestingHistoryOut])
def list_player_testing_history(
    player_id: str,
    metric: str = Query(None),
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    q = (
        sb.table("player_testing_history")
        .select("*")
        .eq("player_id", player_id)
    )
    if metric:
        q = q.eq("metric_name", metric)
    resp = q.order("tested_at", desc=True).execute()
    return resp.data


@router.get("/teams/{team_id}/testing", response_model=List[PlayerTestingOut])
def list_team_testing(team_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("player_testing")
        .select("*, players!inner(team_id)")
        .eq("players.team_id", team_id)
        .execute()
    )
    rows = []
    for row in resp.data:
        row.pop("players", None)
        rows.append(row)
    return rows


@router.delete("/testing/{test_id}", status_code=204)
def delete_testing(test_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    sb.table("player_testing").delete().eq("id", test_id).execute()
