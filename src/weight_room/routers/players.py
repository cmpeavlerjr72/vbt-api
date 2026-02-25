"""Player endpoints."""
from __future__ import annotations

from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.access import require_team_access
from weight_room.core.models import ClaimInviteRequest, PlayerCreate, PlayerMeOut, PlayerOut, PlayerUpdate, TeamOut
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
    require_team_access(sb, team_id, user_id)
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
    require_team_access(sb, team_id, user_id)
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


# ── Static /players/* paths BEFORE the {player_id} wildcard ──────────────

@router.post("/players/claim", response_model=PlayerOut)
def claim_invite(body: ClaimInviteRequest, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    # Use the RPC function for atomic claim — pass uid explicitly since
    # service-role calls have auth.uid() = NULL
    try:
        resp = sb.rpc("claim_invite_code", {"code": body.invite_code, "uid": user_id}).execute()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    if not resp or not resp.data:
        raise HTTPException(status_code=400, detail="Failed to claim invite code")
    return resp.data


@router.get("/players/me", response_model=Optional[PlayerMeOut])
def get_my_player(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("players")
        .select("id, team_id, first_name, last_name, jersey_number, position_group, linked_user_id, linked_at")
        .eq("linked_user_id", user_id)
        .maybe_single()
        .execute()
    )
    if not resp or not resp.data:
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
    player["teams"] = t_resp.data if t_resp else None
    return player


@router.get("/players/me/team", response_model=Optional[TeamOut])
def get_my_team(user_id: str = Depends(get_current_user)):
    """Return the team for the currently linked player. No coach access needed."""
    sb = _require_db()
    # Find the player linked to this user
    p_resp = (
        sb.table("players")
        .select("team_id")
        .eq("linked_user_id", user_id)
        .maybe_single()
        .execute()
    )
    if not p_resp or not p_resp.data:
        return None
    team_id = p_resp.data["team_id"]
    t_resp = sb.table("teams").select("*").eq("id", team_id).maybe_single().execute()
    if not t_resp or not t_resp.data:
        return None
    return t_resp.data


# ── /players/{player_id} wildcard routes ─────────────────────────────────

@router.get("/players/{player_id}", response_model=PlayerOut)
def get_player(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = sb.table("players").select("*").eq("id", player_id).maybe_single().execute()
    if not resp or not resp.data:
        raise HTTPException(status_code=404, detail="Player not found")
    return resp.data


@router.put("/players/{player_id}", response_model=PlayerOut)
def update_player(player_id: str, body: PlayerUpdate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    patch = {k: v for k, v in body.model_dump(exclude_unset=True).items()}
    if not patch:
        raise HTTPException(status_code=400, detail="No fields to update")
    resp = sb.table("players").update(patch).eq("id", player_id).execute()
    if not resp or not resp.data:
        raise HTTPException(status_code=404, detail="Player not found")
    return resp.data[0]


@router.delete("/players/{player_id}", status_code=204)
def delete_player(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    sb.table("players").delete().eq("id", player_id).execute()
