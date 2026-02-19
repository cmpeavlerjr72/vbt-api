"""Player endpoints."""
from __future__ import annotations

from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.models import ClaimInviteRequest, PlayerCreate, PlayerMeOut, PlayerOut, PlayerUpdate
from weight_room.db import get_supabase

router = APIRouter(tags=["players"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("/teams/{team_id}/players", response_model=List[PlayerOut])
def list_team_players(team_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("players")
        .select("*")
        .eq("team_id", team_id)
        .order("created_at")
        .execute()
    )
    return resp.data


@router.post("/teams/{team_id}/players", response_model=PlayerOut, status_code=201)
def create_player(team_id: str, body: PlayerCreate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("players")
        .insert({
            "team_id": team_id,
            "first_name": body.first_name,
            "last_name": body.last_name,
            "jersey_number": body.jersey_number,
            "position_group": body.position_group,
        })
        .execute()
    )
    return resp.data[0]


@router.put("/players/{player_id}", response_model=PlayerOut)
def update_player(player_id: str, body: PlayerUpdate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    patch = {k: v for k, v in body.model_dump(exclude_unset=True).items()}
    if not patch:
        raise HTTPException(status_code=400, detail="No fields to update")
    resp = sb.table("players").update(patch).eq("id", player_id).execute()
    if not resp.data:
        raise HTTPException(status_code=404, detail="Player not found")
    return resp.data[0]


@router.delete("/players/{player_id}", status_code=204)
def delete_player(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    sb.table("players").delete().eq("id", player_id).execute()


@router.post("/players/claim", response_model=PlayerOut)
def claim_invite(body: ClaimInviteRequest, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    # Use the RPC function for atomic claim
    try:
        resp = sb.rpc("claim_invite_code", {"code": body.invite_code}).execute()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    if not resp.data:
        raise HTTPException(status_code=400, detail="Failed to claim invite code")
    return resp.data


@router.get("/players/me", response_model=Optional[PlayerMeOut])
def get_my_player(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("players")
        .select("id, team_id, first_name, last_name, linked_user_id, linked_at")
        .eq("linked_user_id", user_id)
        .maybe_single()
        .execute()
    )
    if not resp.data:
        return None
    player = resp.data

    # Fetch team name
    t_resp = (
        sb.table("teams")
        .select("name")
        .eq("id", player["team_id"])
        .maybe_single()
        .execute()
    )
    player["teams"] = t_resp.data
    return player
