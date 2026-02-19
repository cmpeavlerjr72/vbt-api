"""RFID tag + scan event endpoints."""
from __future__ import annotations

from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Query

from weight_room.auth import get_current_user
from weight_room.core.models import RfidTagAssign, RfidTagCreate, RfidTagOut, ScanEventCreate
from weight_room.db import get_supabase

router = APIRouter(tags=["rfid"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("/rfid/lookup", response_model=Optional[RfidTagOut])
def lookup_rfid_tag(uid: str = Query(...), user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("rfid_tags")
        .select("*")
        .eq("uid", uid)
        .maybe_single()
        .execute()
    )
    return resp.data


@router.post("/rfid/tags", response_model=RfidTagOut, status_code=201)
def create_rfid_tag(body: RfidTagCreate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("rfid_tags")
        .insert({"uid": body.uid, "team_id": body.team_id})
        .execute()
    )
    return resp.data[0]


@router.put("/rfid/tags/{tag_id}/assign", response_model=RfidTagOut)
def assign_rfid_tag(tag_id: str, body: RfidTagAssign, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    # Update the tag
    resp = (
        sb.table("rfid_tags")
        .update({"assigned_player_id": body.player_id})
        .eq("id", tag_id)
        .execute()
    )
    if not resp.data:
        raise HTTPException(status_code=404, detail="Tag not found")

    # Update the player's rfid_tag_id
    sb.table("players").update({"rfid_tag_id": tag_id}).eq("id", body.player_id).execute()

    return resp.data[0]


@router.post("/scan-events", status_code=201)
def create_scan_event(body: ScanEventCreate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    sb.table("scan_events").insert({
        "team_id": body.team_id,
        "uid": body.uid,
        "device_id": body.device_id,
    }).execute()
    return {"ok": True}
