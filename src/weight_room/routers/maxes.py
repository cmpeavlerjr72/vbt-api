"""Player maxes endpoints."""
from __future__ import annotations

from datetime import datetime, timezone
from typing import List

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.models import PlayerMaxOut, PlayerMaxUpsert
from weight_room.db import get_supabase

router = APIRouter(tags=["maxes"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("/players/{player_id}/maxes", response_model=List[PlayerMaxOut])
def list_player_maxes(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("player_maxes")
        .select("*")
        .eq("player_id", player_id)
        .order("exercise")
        .execute()
    )
    return resp.data


@router.post("/players/{player_id}/maxes", response_model=PlayerMaxOut)
def upsert_player_max(
    player_id: str, body: PlayerMaxUpsert, user_id: str = Depends(get_current_user)
):
    sb = _require_db()
    resp = (
        sb.table("player_maxes")
        .upsert(
            {
                "player_id": player_id,
                "exercise": body.exercise,
                "weight": body.weight,
                "tested_at": datetime.now(timezone.utc).isoformat(),
            },
            on_conflict="player_id,exercise",
        )
        .execute()
    )
    return resp.data[0]


@router.get("/teams/{team_id}/maxes", response_model=List[PlayerMaxOut])
def list_team_maxes(team_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("player_maxes")
        .select("*, players!inner(team_id)")
        .eq("players.team_id", team_id)
        .execute()
    )
    # Strip the join data before returning
    rows = []
    for row in resp.data:
        row.pop("players", None)
        rows.append(row)
    return rows


@router.delete("/maxes/{max_id}", status_code=204)
def delete_max(max_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    sb.table("player_maxes").delete().eq("id", max_id).execute()
