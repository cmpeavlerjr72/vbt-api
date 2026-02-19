"""Profile endpoints."""
from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.models import ProfileOut, ProfileUpdate
from weight_room.db import get_supabase

router = APIRouter(prefix="/profiles", tags=["profiles"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("/me", response_model=ProfileOut)
def get_my_profile(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = sb.table("profiles").select("*").eq("id", user_id).maybe_single().execute()
    if not resp.data:
        raise HTTPException(status_code=404, detail="Profile not found")
    return resp.data


@router.put("/me", response_model=ProfileOut)
def update_my_profile(body: ProfileUpdate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    patch = {k: v for k, v in body.model_dump(exclude_unset=True).items()}
    if not patch:
        raise HTTPException(status_code=400, detail="No fields to update")
    resp = sb.table("profiles").update(patch).eq("id", user_id).execute()
    if not resp.data:
        raise HTTPException(status_code=404, detail="Profile not found")
    return resp.data[0]
