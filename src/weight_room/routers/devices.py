"""Coach-side device pairing endpoints."""
from __future__ import annotations

import logging
from typing import List

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.models import DeviceOut, DeviceRegister
from weight_room.db import get_supabase

log = logging.getLogger(__name__)

router = APIRouter(prefix="/coach/devices", tags=["coach_devices"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.get("", response_model=List[DeviceOut])
def list_my_devices(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    try:
        resp = (
            sb.table("devices")
            .select("id, device_id, coach_id, label, created_at")
            .eq("coach_id", user_id)
            .order("created_at")
            .execute()
        )
    except Exception as exc:
        log.exception("list devices failed coach=%s", user_id)
        raise HTTPException(status_code=500, detail=f"List devices failed: {exc}")
    return resp.data or []


@router.post("", response_model=DeviceOut, status_code=201)
def pair_device(body: DeviceRegister, user_id: str = Depends(get_current_user)):
    sb = _require_db()

    device_id = body.device_id.strip()
    if not device_id:
        raise HTTPException(status_code=400, detail="device_id required")

    # Reject if already claimed by anyone (re-pair = unpair first).
    try:
        existing = (
            sb.table("devices")
            .select("coach_id")
            .eq("device_id", device_id)
            .maybe_single()
            .execute()
        )
    except Exception as exc:
        log.exception("device existence check failed device=%s", device_id)
        raise HTTPException(status_code=500, detail=f"Pairing check failed: {exc}")

    if existing and existing.data:
        if existing.data.get("coach_id") == user_id:
            raise HTTPException(status_code=409, detail="Device already paired to you")
        if existing.data.get("coach_id"):
            raise HTTPException(status_code=409, detail="Device already paired to another coach")

    try:
        resp = (
            sb.table("devices")
            .insert({
                "device_id": device_id,
                "coach_id": user_id,
                "label": body.label,
            })
            .execute()
        )
    except Exception as exc:
        log.exception("device pair failed device=%s coach=%s", device_id, user_id)
        raise HTTPException(status_code=500, detail=f"Pairing failed: {exc}")

    if not resp or not resp.data:
        raise HTTPException(status_code=500, detail="Pairing returned no row")

    return resp.data[0]


@router.delete("/{device_row_id}", status_code=204)
def unpair_device(device_row_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    try:
        resp = (
            sb.table("devices")
            .delete()
            .eq("id", device_row_id)
            .eq("coach_id", user_id)
            .execute()
        )
    except Exception as exc:
        log.exception("device unpair failed id=%s coach=%s", device_row_id, user_id)
        raise HTTPException(status_code=500, detail=f"Unpair failed: {exc}")

    if not resp or not resp.data:
        raise HTTPException(status_code=404, detail="Device not found")
    return None
