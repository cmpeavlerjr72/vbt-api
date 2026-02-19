"""Team endpoints."""
from __future__ import annotations

from typing import List

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.models import TeamCreate, TeamOut, TeamUpdate
from weight_room.db import get_supabase

router = APIRouter(prefix="/teams", tags=["teams"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("", response_model=List[TeamOut])
def list_teams(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("teams")
        .select("*")
        .eq("coach_id", user_id)
        .order("created_at", desc=True)
        .execute()
    )
    return resp.data


@router.get("/{team_id}", response_model=TeamOut)
def get_team(team_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = sb.table("teams").select("*").eq("id", team_id).maybe_single().execute()
    if not resp.data:
        raise HTTPException(status_code=404, detail="Team not found")
    return resp.data


@router.post("", response_model=TeamOut, status_code=201)
def create_team(body: TeamCreate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("teams")
        .insert({"coach_id": user_id, "name": body.name, "sport": body.sport})
        .execute()
    )
    return resp.data[0]


@router.put("/{team_id}", response_model=TeamOut)
def update_team(team_id: str, body: TeamUpdate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    patch = {k: v for k, v in body.model_dump(exclude_unset=True).items()}
    if not patch:
        raise HTTPException(status_code=400, detail="No fields to update")
    resp = (
        sb.table("teams")
        .update(patch)
        .eq("id", team_id)
        .eq("coach_id", user_id)
        .execute()
    )
    if not resp.data:
        raise HTTPException(status_code=404, detail="Team not found")
    return resp.data[0]
