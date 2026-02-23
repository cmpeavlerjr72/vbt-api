"""Team coaches endpoints — list, add, remove coaches on a team."""
from __future__ import annotations

from typing import List

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.access import require_team_access, require_team_owner
from weight_room.core.models import AddTeamCoachRequest, TeamCoachOut
from weight_room.db import get_supabase

router = APIRouter(prefix="/teams/{team_id}/coaches", tags=["team_coaches"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("", response_model=List[TeamCoachOut])
def list_team_coaches(team_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    require_team_access(sb, team_id, user_id)

    rows = (
        sb.table("team_coaches")
        .select("id, team_id, coach_id, role, added_at")
        .eq("team_id", team_id)
        .order("added_at")
        .execute()
        .data
    )

    # Enrich with profile info
    coach_ids = [r["coach_id"] for r in rows]
    profiles = {}
    if coach_ids:
        p_rows = (
            sb.table("profiles")
            .select("id, display_name, email")
            .in_("id", coach_ids)
            .execute()
            .data
        )
        profiles = {p["id"]: p for p in p_rows}

    result = []
    for r in rows:
        p = profiles.get(r["coach_id"], {})
        result.append({
            **r,
            "display_name": p.get("display_name"),
            "email": p.get("email"),
        })
    return result


@router.post("", response_model=TeamCoachOut, status_code=201)
def add_team_coach(
    team_id: str,
    body: AddTeamCoachRequest,
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    require_team_owner(sb, team_id, user_id)

    # Look up profile by coach_code
    profile = (
        sb.table("profiles")
        .select("id, display_name, email")
        .eq("coach_code", body.coach_code)
        .maybe_single()
        .execute()
    )
    if not profile.data:
        raise HTTPException(status_code=404, detail="No coach found with that code")

    target_id = profile.data["id"]
    if target_id == user_id:
        raise HTTPException(status_code=400, detail="You are already the head coach")

    # Prevent duplicates
    existing = (
        sb.table("team_coaches")
        .select("id")
        .eq("team_id", team_id)
        .eq("coach_id", target_id)
        .maybe_single()
        .execute()
    )
    if existing.data:
        raise HTTPException(status_code=409, detail="Coach is already on this team")

    resp = (
        sb.table("team_coaches")
        .insert({"team_id": team_id, "coach_id": target_id, "role": "assistant"})
        .execute()
    )
    row = resp.data[0]
    return {
        **row,
        "display_name": profile.data.get("display_name"),
        "email": profile.data.get("email"),
    }


@router.delete("/{coach_id}", status_code=204)
def remove_team_coach(
    team_id: str,
    coach_id: str,
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    team = require_team_owner(sb, team_id, user_id)

    if coach_id == team["coach_id"]:
        raise HTTPException(status_code=400, detail="Cannot remove the head coach")

    sb.table("team_coaches").delete().eq("team_id", team_id).eq("coach_id", coach_id).execute()
