"""Workout template + assignment endpoints."""
from __future__ import annotations

from typing import List

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.models import (
    WorkoutAssignmentCreate,
    WorkoutAssignmentOut,
    WorkoutTemplateCreate,
    WorkoutTemplateOut,
    WorkoutTemplateUpdate,
)
from weight_room.db import get_supabase

router = APIRouter(tags=["workouts"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


# ── Templates ────────────────────────────────────────────────────────────────

@router.get("/templates", response_model=List[WorkoutTemplateOut])
def list_templates(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("workout_templates")
        .select("*")
        .eq("coach_id", user_id)
        .order("created_at", desc=True)
        .execute()
    )
    return resp.data


@router.post("/templates", response_model=WorkoutTemplateOut, status_code=201)
def create_template(body: WorkoutTemplateCreate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("workout_templates")
        .insert({
            "coach_id": user_id,
            "sport": body.sport,
            "name": body.name,
            "description": body.description,
            "content": body.content or {"version": 2, "exercises": []},
        })
        .execute()
    )
    return resp.data[0]


@router.put("/templates/{template_id}", response_model=WorkoutTemplateOut)
def update_template(
    template_id: str,
    body: WorkoutTemplateUpdate,
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    patch = {k: v for k, v in body.model_dump(exclude_unset=True).items()}
    if not patch:
        raise HTTPException(status_code=400, detail="No fields to update")
    resp = (
        sb.table("workout_templates")
        .update(patch)
        .eq("id", template_id)
        .eq("coach_id", user_id)
        .execute()
    )
    if not resp.data:
        raise HTTPException(status_code=404, detail="Template not found")
    return resp.data[0]


@router.delete("/templates/{template_id}", status_code=204)
def delete_template(template_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    sb.table("workout_templates").delete().eq("id", template_id).eq("coach_id", user_id).execute()


# ── Assignments ──────────────────────────────────────────────────────────────

@router.get("/teams/{team_id}/assignments", response_model=List[WorkoutAssignmentOut])
def list_team_assignments(team_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = (
        sb.table("workout_assignments")
        .select("*")
        .eq("team_id", team_id)
        .order("created_at", desc=True)
        .execute()
    )
    return resp.data


@router.post("/assignments", response_model=WorkoutAssignmentOut, status_code=201)
def create_assignment(body: WorkoutAssignmentCreate, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    row = {
        "team_id": body.team_id,
        "template_id": body.template_id,
        "target_type": body.target_type,
        "target_position_group": (
            body.target_position_group
            if body.target_type == "position_group"
            else None
        ),
        "start_at": body.start_at,
        "due_at": body.due_at,
        "notes": body.notes,
        "created_by": user_id,
    }
    resp = sb.table("workout_assignments").insert(row).execute()
    assignment = resp.data[0]

    # Insert junction rows for player-specific assignments
    if body.target_type == "players" and body.player_ids:
        junction_rows = [
            {"assignment_id": assignment["id"], "player_id": pid}
            for pid in body.player_ids
        ]
        sb.table("workout_assignment_players").insert(junction_rows).execute()

    return assignment
