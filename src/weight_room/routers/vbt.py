"""VBT (velocity-based training) data endpoints."""
from __future__ import annotations

from typing import List

from fastapi import APIRouter, Depends, HTTPException, Query, Response

from weight_room.auth import get_current_user
from weight_room.core.models import VbtRepOut, VbtSetSummaryOut
from weight_room.db import get_supabase

router = APIRouter(tags=["vbt"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("/players/{player_id}/vbt/set-summaries", response_model=List[VbtSetSummaryOut])
def player_set_summaries(
    player_id: str,
    limit: int = Query(20, ge=1, le=100),
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    resp = (
        sb.table("vbt_set_summaries")
        .select("*")
        .eq("player_id", player_id)
        .order("created_at", desc=True)
        .limit(limit)
        .execute()
    )
    return resp.data


@router.get("/vbt/sets/{raw_set_id}/reps", response_model=List[VbtRepOut])
def set_reps(raw_set_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("vbt_reps")
        .select("*")
        .eq("raw_set_id", raw_set_id)
        .order("rep_number")
        .execute()
    )
    return resp.data


@router.get("/players/{player_id}/vbt/recent-reps", response_model=List[VbtRepOut])
def player_recent_reps(
    player_id: str,
    limit: int = Query(50, ge=1, le=200),
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    resp = (
        sb.table("vbt_reps")
        .select("*")
        .eq("player_id", player_id)
        .order("created_at", desc=True)
        .limit(limit)
        .execute()
    )
    return resp.data


@router.get("/teams/{team_id}/vbt/set-summaries", response_model=List[VbtSetSummaryOut])
def team_set_summaries(
    team_id: str,
    limit: int = Query(50, ge=1, le=200),
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    resp = (
        sb.table("vbt_set_summaries")
        .select("*, vbt_raw_sets!inner(team_id)")
        .eq("vbt_raw_sets.team_id", team_id)
        .order("created_at", desc=True)
        .limit(limit)
        .execute()
    )
    # Strip join data
    rows = []
    for row in resp.data:
        row.pop("vbt_raw_sets", None)
        rows.append(row)
    return rows


@router.get("/teams/{team_id}/vbt/flagged-reps", response_model=List[VbtRepOut])
def team_flagged_reps(
    team_id: str,
    limit: int = Query(30, ge=1, le=100),
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    resp = (
        sb.table("vbt_reps")
        .select("*, vbt_raw_sets!inner(team_id)")
        .eq("vbt_raw_sets.team_id", team_id)
        .eq("flagged", True)
        .order("created_at", desc=True)
        .limit(limit)
        .execute()
    )
    rows = []
    for row in resp.data:
        row.pop("vbt_raw_sets", None)
        rows.append(row)
    return rows


@router.get("/players/{player_id}/vbt/prs", response_model=List[VbtRepOut])
def player_prs(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("vbt_reps")
        .select("*")
        .eq("player_id", player_id)
        .order("mean_velocity", desc=True)
        .execute()
    )
    # Dedupe to best per exercise
    best: dict[str, dict] = {}
    for rep in resp.data:
        ex = rep["exercise"]
        if ex not in best:
            best[ex] = rep
    return list(best.values())


@router.delete("/vbt/sets/{raw_set_id}", status_code=204)
def delete_set(raw_set_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()

    # Verify the set exists and belongs to a team coached by this user
    set_resp = (
        sb.table("vbt_raw_sets")
        .select("id, team_id")
        .eq("id", raw_set_id)
        .execute()
    )
    if not set_resp.data:
        raise HTTPException(status_code=404, detail="Set not found")

    team_id = set_resp.data[0]["team_id"]
    team_resp = (
        sb.table("teams")
        .select("coach_id")
        .eq("id", team_id)
        .execute()
    )
    if not team_resp.data or team_resp.data[0]["coach_id"] != user_id:
        raise HTTPException(status_code=403, detail="Not authorized to delete this set")

    # Delete from vbt_raw_sets — CASCADE handles reps + summaries
    sb.table("vbt_raw_sets").delete().eq("id", raw_set_id).execute()

    return Response(status_code=204)
