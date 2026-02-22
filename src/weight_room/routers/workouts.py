"""Workout template + assignment endpoints."""
from __future__ import annotations

from typing import List

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.models import (
    ActiveWorkout,
    ExerciseProgress,
    PlayerProgress,
    WorkoutAssignmentCreate,
    WorkoutAssignmentOut,
    WorkoutExerciseLogIn,
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


@router.delete("/assignments/{assignment_id}", status_code=204)
def delete_assignment(assignment_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    # Delete junction rows first, then the assignment itself
    sb.table("workout_assignment_players").delete().eq("assignment_id", assignment_id).execute()
    sb.table("workout_assignments").delete().eq("id", assignment_id).execute()


# ── Workout Logging ─────────────────────────────────────────────────────

# Exercises the firmware/device supports — used to default trackingMode
_VBT_EXERCISES = {
    "Back Squat", "Front Squat", "Bench Press", "Overhead Press",
    "Deadlift", "Trap Bar Deadlift", "Romanian Deadlift",
    "Power Clean", "Hang Clean", "Push Press",
}


def _parse_exercises(content: dict) -> list[dict]:
    """Extract exercises from a template's content JSONB, handling v1 and v2."""
    exercises = content.get("exercises", [])
    result = []
    is_v2 = content.get("version") == 2
    for ex in exercises:
        if is_v2:
            name = ex.get("exerciseName", "")
            set_groups = ex.get("setGroups", [])
            tracking = ex.get("trackingMode")
            if tracking is None:
                tracking = "vbt" if name in _VBT_EXERCISES else "self_report"
            total_sets = sum(sg.get("sets", 0) for sg in set_groups)
        else:
            name = ex.get("name", "")
            tracking = "vbt" if name in _VBT_EXERCISES else "self_report"
            total_sets = ex.get("sets", 0)
        result.append({
            "exercise_name": name,
            "tracking_mode": tracking,
            "sets_required": total_sets,
        })
    return result


def _compute_exercise_progress(
    sb, parsed_exercises: list[dict], player_id: str, assignment_id: str,
    start_at: str | None, due_at: str | None, created_at: str,
) -> list[ExerciseProgress]:
    """For each exercise, compute sets_completed from VBT or self-report data."""
    progress = []
    for ex in parsed_exercises:
        name = ex["exercise_name"]
        mode = ex["tracking_mode"]
        sets_req = ex["sets_required"]
        weight = None
        reps = None
        sets_done = 0

        if mode == "vbt":
            # Count vbt_set_summaries matching player + exercise within the window
            window_start = start_at or created_at
            q = (
                sb.table("vbt_set_summaries")
                .select("id", count="exact")
                .eq("player_id", player_id)
                .eq("exercise", name)
                .gte("created_at", window_start)
            )
            if due_at:
                q = q.lte("created_at", due_at)
            resp = q.execute()
            sets_done = resp.count if resp.count is not None else len(resp.data)
        else:
            # Self-report: fetch from workout_exercise_logs
            resp = (
                sb.table("workout_exercise_logs")
                .select("*")
                .eq("assignment_id", assignment_id)
                .eq("player_id", player_id)
                .eq("exercise_name", name)
                .execute()
            )
            if resp.data:
                row = resp.data[0]
                sets_done = row.get("sets_completed", 0)
                weight = row.get("weight_lbs")
                reps = row.get("reps_per_set")

        progress.append(ExerciseProgress(
            exercise_name=name,
            tracking_mode=mode,
            sets_required=sets_req,
            sets_completed=sets_done,
            weight_lbs=weight,
            reps_per_set=reps,
        ))
    return progress


@router.get(
    "/players/{player_id}/active-workouts",
    response_model=List[ActiveWorkout],
)
def get_active_workouts(player_id: str, user_id: str = Depends(get_current_user)):
    """Return player's active workout assignments with per-exercise completion."""
    sb = _require_db()

    # Get player row
    player_resp = sb.table("players").select("*").eq("id", player_id).execute()
    if not player_resp.data:
        raise HTTPException(status_code=404, detail="Player not found")
    player = player_resp.data[0]

    # Get active assignments for the player's team
    assign_resp = (
        sb.table("workout_assignments")
        .select("*")
        .eq("team_id", player["team_id"])
        .eq("status", "active")
        .order("created_at", desc=True)
        .execute()
    )

    results: list[ActiveWorkout] = []
    for assignment in assign_resp.data:
        # Filter by target
        target = assignment.get("target_type", "team")
        if target == "position_group":
            if assignment.get("target_position_group") != player.get("position_group"):
                continue
        elif target == "players":
            junction = (
                sb.table("workout_assignment_players")
                .select("player_id")
                .eq("assignment_id", assignment["id"])
                .eq("player_id", player_id)
                .execute()
            )
            if not junction.data:
                continue

        # Get template
        tmpl_resp = (
            sb.table("workout_templates")
            .select("*")
            .eq("id", assignment["template_id"])
            .execute()
        )
        if not tmpl_resp.data:
            continue
        tmpl = tmpl_resp.data[0]

        parsed = _parse_exercises(tmpl.get("content", {}))
        exercises = _compute_exercise_progress(
            sb, parsed, player_id, assignment["id"],
            assignment.get("start_at"), assignment.get("due_at"),
            assignment["created_at"],
        )

        results.append(ActiveWorkout(
            assignment_id=assignment["id"],
            template_name=tmpl["name"],
            due_at=assignment.get("due_at"),
            exercises=exercises,
        ))

    return results


@router.put("/players/{player_id}/workout-log/{assignment_id}")
def submit_workout_log(
    player_id: str,
    assignment_id: str,
    body: List[WorkoutExerciseLogIn],
    user_id: str = Depends(get_current_user),
):
    """Upsert self-reported exercise logs for a player's assignment."""
    sb = _require_db()

    rows = [
        {
            "assignment_id": assignment_id,
            "player_id": player_id,
            "exercise_name": log.exercise_name,
            "weight_lbs": log.weight_lbs,
            "sets_completed": log.sets_completed,
            "reps_per_set": log.reps_per_set,
            "notes": log.notes,
            "logged_at": "now()",
        }
        for log in body
    ]

    sb.table("workout_exercise_logs").upsert(
        rows,
        on_conflict="assignment_id,player_id,exercise_name",
    ).execute()

    return {"ok": True}


@router.get(
    "/assignments/{assignment_id}/progress",
    response_model=List[PlayerProgress],
)
def get_assignment_progress(
    assignment_id: str,
    user_id: str = Depends(get_current_user),
):
    """Coach endpoint: completion grid for all eligible players on an assignment."""
    sb = _require_db()

    # Get assignment
    assign_resp = (
        sb.table("workout_assignments")
        .select("*")
        .eq("id", assignment_id)
        .execute()
    )
    if not assign_resp.data:
        raise HTTPException(status_code=404, detail="Assignment not found")
    assignment = assign_resp.data[0]

    # Verify coach owns the team
    team_resp = (
        sb.table("teams")
        .select("*")
        .eq("id", assignment["team_id"])
        .eq("coach_id", user_id)
        .execute()
    )
    if not team_resp.data:
        raise HTTPException(status_code=403, detail="Not your team")

    # Get template content
    tmpl_resp = (
        sb.table("workout_templates")
        .select("*")
        .eq("id", assignment["template_id"])
        .execute()
    )
    if not tmpl_resp.data:
        raise HTTPException(status_code=404, detail="Template not found")
    parsed = _parse_exercises(tmpl_resp.data[0].get("content", {}))

    # Get eligible players
    target = assignment.get("target_type", "team")
    player_q = sb.table("players").select("*").eq("team_id", assignment["team_id"])
    if target == "position_group":
        player_q = player_q.eq("position_group", assignment.get("target_position_group"))
    players_resp = player_q.execute()

    if target == "players":
        junction_resp = (
            sb.table("workout_assignment_players")
            .select("player_id")
            .eq("assignment_id", assignment_id)
            .execute()
        )
        eligible_ids = {r["player_id"] for r in junction_resp.data}
        all_players = [p for p in players_resp.data if p["id"] in eligible_ids]
    else:
        all_players = players_resp.data

    results: list[PlayerProgress] = []
    for player in all_players:
        exercises = _compute_exercise_progress(
            sb, parsed, player["id"], assignment_id,
            assignment.get("start_at"), assignment.get("due_at"),
            assignment["created_at"],
        )
        results.append(PlayerProgress(
            player_id=player["id"],
            player_name=f"{player.get('first_name', '')} {player.get('last_name', '')}".strip(),
            jersey_number=player.get("jersey_number"),
            position_group=player.get("position_group", ""),
            exercises=exercises,
        ))

    return results
