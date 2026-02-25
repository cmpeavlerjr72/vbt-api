"""Dashboard endpoints.

Real coach dashboard data from DB queries.
Team/player dashboards still return mock data.
"""
from __future__ import annotations

import logging
from datetime import datetime, timedelta, timezone
from typing import Dict, List, Optional

from fastapi import APIRouter, Depends, HTTPException, Query

from weight_room.auth import get_current_user
from weight_room.core.access import get_all_accessible_teams, require_team_access
from weight_room.core.models import (
    ActivityItem,
    DueWorkout,
    LeaderboardEntry,
    LivePlayerActivity,
    PersonalRecord,
    PositionComparison,
    StatCard,
    TeamOverview,
    TeamOverviewDetail,
    TodayWorkout,
    TodayExercise,
    TodaySetGroup,
    TrendPoint,
    WorkoutSession,
)
from weight_room.db import get_supabase
from weight_room.routers.workouts import _parse_exercises

logger = logging.getLogger(__name__)

router = APIRouter(tags=["dashboard"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


# ─── Shared helpers ──────────────────────────────────────────────────────────

def _get_coach_context(sb, user_id: str, *, include_archived: bool = False):
    """Fetch teams, team_ids, and players for a coach (includes assistant teams)."""
    teams = get_all_accessible_teams(sb, user_id, include_archived=include_archived)
    team_ids = [t["id"] for t in teams]
    players: list = []
    if team_ids:
        _p_resp = sb.table("players").select("*").in_("team_id", team_ids).execute()
        players = _p_resp.data if _p_resp else []
    return teams, team_ids, players


def _week_bounds() -> tuple:
    """Return (monday_iso, next_monday_iso) for the current week (Mon-Sun)."""
    now = datetime.now(timezone.utc)
    monday = now.replace(hour=0, minute=0, second=0, microsecond=0) - timedelta(
        days=now.weekday()
    )
    return monday.isoformat(), (monday + timedelta(days=7)).isoformat()


def _players_by_team(players: list) -> Dict[str, list]:
    """Group players into {team_id: [player, ...]}."""
    result: Dict[str, list] = {}
    for p in players:
        result.setdefault(p["team_id"], []).append(p)
    return result


def _eligible_player_ids(
    assignment: dict, pbt: Dict[str, list], junction_map: Dict[str, set]
) -> set:
    """Return set of player IDs eligible for an assignment."""
    team_players = pbt.get(assignment["team_id"], [])
    target = assignment.get("target_type", "team")
    if target == "position_group":
        pg = assignment.get("target_position_group")
        return {p["id"] for p in team_players if p.get("position_group") == pg}
    if target == "players":
        return junction_map.get(assignment["id"], set())
    return {p["id"] for p in team_players}


def _assignment_vbt_window(assignment, fallback_start, fallback_end):
    """Derive VBT matching window for an assignment.

    Matches the logic in _compute_exercise_progress (workouts.py):
    - start: start_at, or start-of-day of due_at, or created_at
    - end:   end-of-day of due_at, or fallback
    """
    start_at = assignment.get("start_at")
    due_at = assignment.get("due_at")
    created_at = assignment.get("created_at")

    if start_at:
        window_start = start_at
    elif due_at:
        window_start = due_at[:10] + "T00:00:00+00:00"
    elif created_at:
        window_start = created_at
    else:
        window_start = fallback_start

    if due_at:
        window_end = due_at[:10] + "T23:59:59+00:00"
    else:
        window_end = fallback_end

    return window_start, window_end


def _fetch_junction_map(sb, assignments: list) -> Dict[str, set]:
    """Batch-fetch workout_assignment_players for player-targeted assignments."""
    ids = [a["id"] for a in assignments if a.get("target_type") == "players"]
    if not ids:
        return {}
    junction: Dict[str, set] = {}
    _junc_resp = (
        sb.table("workout_assignment_players")
        .select("assignment_id, player_id")
        .in_("assignment_id", ids)
        .execute()
    )
    for row in (_junc_resp.data if _junc_resp else []):
        junction.setdefault(row["assignment_id"], set()).add(row["player_id"])
    return junction


def _compute_set_completion(sb, assignments, players, junction_map, since_iso, until_iso):
    """Compute set-level completion for each assignment.

    Returns dict: {assignment_id: (sets_done, sets_total, team_id)}
    where totals are summed across all eligible players.
    Matches the per-exercise logic in _compute_exercise_progress (workouts.py).
    """
    if not assignments:
        return {}

    pbt = _players_by_team(players)
    assignment_ids = [a["id"] for a in assignments]

    # Self-report logs: (assignment_id, player_id, exercise_name) -> sets_completed
    log_data: Dict[tuple, int] = {}
    try:
        _log_resp = (
            sb.table("workout_exercise_logs")
            .select("assignment_id, player_id, exercise_name, sets_completed")
            .in_("assignment_id", assignment_ids)
            .execute()
        )
        for row in (_log_resp.data if _log_resp else []):
            key = (row["assignment_id"], row["player_id"], row["exercise_name"])
            log_data[key] = row.get("sets_completed", 0)
    except Exception:
        pass  # table may not exist yet

    # VBT activity with exercise names, indexed for fast lookup
    player_ids = [p["id"] for p in players]
    vbt_index: Dict[tuple, list] = {}  # (player_id, exercise) -> [created_at, ...]

    if player_ids:
        # Compute broad query window from all assignment windows
        all_windows = [
            _assignment_vbt_window(a, since_iso, until_iso) for a in assignments
        ]
        earliest = min(w[0] for w in all_windows)
        latest = max(w[1] for w in all_windows)

        _vbt_resp = (
            sb.table("vbt_set_summaries")
            .select("player_id, exercise, created_at")
            .in_("player_id", player_ids)
            .gte("created_at", earliest)
            .lte("created_at", latest)
            .execute()
        )
        vbt_rows = _vbt_resp.data if _vbt_resp else []
        for v in vbt_rows:
            vbt_index.setdefault((v["player_id"], v["exercise"]), []).append(
                v["created_at"]
            )

    result: Dict[str, tuple] = {}

    for a in assignments:
        eligible = _eligible_player_ids(a, pbt, junction_map)

        # Parse template exercises
        template = a.get("workout_templates")
        content = (
            template.get("content", {})
            if isinstance(template, dict) and template
            else {}
        )
        exercises = _parse_exercises(content)

        sets_per_player = sum(ex["sets_required"] for ex in exercises)
        total_required = sets_per_player * len(eligible)

        w_start, w_end = _assignment_vbt_window(a, since_iso, until_iso)

        total_done = 0
        for pid in eligible:
            for ex in exercises:
                name = ex["exercise_name"]
                req = ex["sets_required"]
                if ex["tracking_mode"] == "vbt":
                    timestamps = vbt_index.get((pid, name), [])
                    done = sum(
                        1 for ts in timestamps if w_start <= ts <= w_end
                    )
                    total_done += min(done, req)
                else:
                    done = log_data.get((a["id"], pid, name), 0)
                    total_done += min(done, req)

        result[a["id"]] = (total_done, total_required, a["team_id"])

    return result


def _compute_compliance(sb, team_ids, players, since_iso, until_iso):
    """
    Set-level compliance percentages: per-team dict and overall pct.

    Counts actual sets completed vs sets required across all eligible
    players, matching the calendar/completion page logic.
    """
    if not team_ids:
        return {}, 0

    _assign_resp = (
        sb.table("workout_assignments")
        .select(
            "id, team_id, target_type, target_position_group, "
            "start_at, due_at, created_at, workout_templates(content)"
        )
        .in_("team_id", team_ids)
        .gte("due_at", since_iso)
        .lte("due_at", until_iso)
        .execute()
    )
    assignments = _assign_resp.data if _assign_resp else []
    if not assignments:
        return {tid: 0 for tid in team_ids}, 0

    junction_map = _fetch_junction_map(sb, assignments)
    completion = _compute_set_completion(
        sb, assignments, players, junction_map, since_iso, until_iso
    )

    # Aggregate by team
    team_done: Dict[str, int] = {tid: 0 for tid in team_ids}
    team_total: Dict[str, int] = {tid: 0 for tid in team_ids}

    for _aid, (done, total, tid) in completion.items():
        team_done[tid] = team_done.get(tid, 0) + done
        team_total[tid] = team_total.get(tid, 0) + total

    per_team = {
        tid: round(team_done[tid] / team_total[tid] * 100)
        if team_total[tid]
        else 0
        for tid in team_ids
    }
    total_d = sum(team_done.values())
    total_t = sum(team_total.values())
    overall = round(total_d / total_t * 100) if total_t else 0
    return per_team, overall


# ─── Coach Dashboard ────────────────────────────────────────────────────────


@router.get("/coach/stats", response_model=List[StatCard])
def coach_stats(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    teams, team_ids, players = _get_coach_context(sb, user_id)

    total_players = len(players)
    active_players = sum(1 for p in players if p.get("linked_user_id"))

    if not team_ids:
        return [
            StatCard(label="Active Players", value=0, subtext="of 0 total", color="blue"),
            StatCard(label="Assigned This Week", value=0, subtext="across all teams", color="blue"),
            StatCard(label="Compliance Rate", value="0%", subtext="no assignments yet", color="green"),
            StatCard(label="Flagged Sessions", value=0, subtext="last 7 days", color="blue"),
        ]

    # Assignments created this week
    monday, _ = _week_bounds()
    _aw_resp = (
        sb.table("workout_assignments")
        .select("id")
        .in_("team_id", team_ids)
        .gte("created_at", monday)
        .execute()
    )
    assigned_this_week = len(_aw_resp.data if _aw_resp else [])

    # Flagged VBT sets in last 7 days
    week_ago = (datetime.now(timezone.utc) - timedelta(days=7)).isoformat()
    player_ids = [p["id"] for p in players]
    flagged_count = 0
    if player_ids:
        _fc_resp = (
            sb.table("vbt_set_summaries")
            .select("id")
            .in_("player_id", player_ids)
            .eq("flagged", True)
            .gte("created_at", week_ago)
            .execute()
        )
        flagged_count = len(_fc_resp.data if _fc_resp else [])

    # Compliance (last 14 days)
    two_weeks_ago = (datetime.now(timezone.utc) - timedelta(days=14)).isoformat()
    now_iso = datetime.now(timezone.utc).isoformat()
    _, compliance_pct = _compute_compliance(
        sb, team_ids, players, two_weeks_ago, now_iso
    )

    return [
        StatCard(
            label="Active Players",
            value=active_players,
            subtext=f"of {total_players} total",
            color="blue",
        ),
        StatCard(
            label="Assigned This Week",
            value=assigned_this_week,
            subtext="across all teams",
            color="blue",
        ),
        StatCard(
            label="Compliance Rate",
            value=f"{compliance_pct}%",
            subtext="last 14 days",
            color="green" if compliance_pct >= 70 else "yellow" if compliance_pct >= 50 else "red",
        ),
        StatCard(
            label="Flagged Sessions",
            value=flagged_count,
            subtext="need form review" if flagged_count else "last 7 days",
            color="red" if flagged_count else "blue",
        ),
    ]


@router.get("/coach/team-overviews", response_model=List[TeamOverview])
def coach_team_overviews(
    include_archived: bool = Query(False),
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    teams, team_ids, players = _get_coach_context(
        sb, user_id, include_archived=include_archived
    )

    if not team_ids:
        return []

    pbt = _players_by_team(players)
    monday, next_monday = _week_bounds()
    two_weeks_ago = (datetime.now(timezone.utc) - timedelta(days=14)).isoformat()
    now_iso = datetime.now(timezone.utc).isoformat()
    week_ago = (datetime.now(timezone.utc) - timedelta(days=7)).isoformat()

    # Assignments due this week per team
    _wa_resp = (
        sb.table("workout_assignments")
        .select("id, team_id")
        .in_("team_id", team_ids)
        .gte("due_at", monday)
        .lt("due_at", next_monday)
        .execute()
    )
    week_assignments = _wa_resp.data if _wa_resp else []
    workouts_per_team: Dict[str, int] = {}
    for a in week_assignments:
        workouts_per_team[a["team_id"]] = workouts_per_team.get(a["team_id"], 0) + 1

    # Flagged VBT sets per team (last 7 days, via player -> team mapping)
    player_team = {p["id"]: p["team_id"] for p in players}
    player_ids = [p["id"] for p in players]
    flagged_per_team: Dict[str, int] = {}
    if player_ids:
        _fr_resp = (
            sb.table("vbt_set_summaries")
            .select("player_id")
            .in_("player_id", player_ids)
            .eq("flagged", True)
            .gte("created_at", week_ago)
            .execute()
        )
        flagged_rows = _fr_resp.data if _fr_resp else []
        for row in flagged_rows:
            tid = player_team.get(row["player_id"])
            if tid:
                flagged_per_team[tid] = flagged_per_team.get(tid, 0) + 1

    # Compliance per team (last 14 days)
    compliance_per_team, _ = _compute_compliance(
        sb, team_ids, players, two_weeks_ago, now_iso
    )

    return [
        TeamOverview(
            id=t["id"],
            name=t["name"],
            sport=t["sport"],
            playerCount=len(pbt.get(t["id"], [])),
            activeCount=sum(
                1 for p in pbt.get(t["id"], []) if p.get("linked_user_id")
            ),
            workoutsThisWeek=workouts_per_team.get(t["id"], 0),
            compliancePercent=compliance_per_team.get(t["id"], 0),
            needsAttention=flagged_per_team.get(t["id"], 0),
            archived=t.get("archived", False),
        )
        for t in teams
    ]


def _build_activity_feed(
    sb, player_ids: list, player_names: dict, limit: int = 15
) -> List[ActivityItem]:
    """Build activity feed from VBT + self-report data for given players."""
    if not player_ids:
        return []

    items: List[ActivityItem] = []

    # VBT activity (most recent)
    _vbt_feed_resp = (
        sb.table("vbt_set_summaries")
        .select(
            "id, player_id, exercise, rep_count, avg_velocity, "
            "peak_velocity, flagged, flag_reason, created_at"
        )
        .in_("player_id", player_ids)
        .order("created_at", desc=True)
        .limit(limit)
        .execute()
    )
    vbt_rows = _vbt_feed_resp.data if _vbt_feed_resp else []
    for row in vbt_rows:
        items.append(
            ActivityItem(
                id=f"vbt-{row['id']}",
                playerName=player_names.get(row["player_id"], "Unknown"),
                exercise=row["exercise"],
                details=(
                    f"{row['rep_count']} reps @ {float(row['avg_velocity']):.2f} m/s avg, "
                    f"{float(row['peak_velocity']):.2f} m/s peak"
                ),
                timestamp=row["created_at"],
                flagged=row.get("flagged", False),
                flagReason=row.get("flag_reason"),
            )
        )

    # Self-report activity — graceful if table missing
    try:
        _log_feed_resp = (
            sb.table("workout_exercise_logs")
            .select(
                "id, player_id, exercise_name, weight_lbs, "
                "sets_completed, reps_per_set, logged_at"
            )
            .in_("player_id", player_ids)
            .order("logged_at", desc=True)
            .limit(limit)
            .execute()
        )
        log_rows = _log_feed_resp.data if _log_feed_resp else []
        for row in log_rows:
            weight = row.get("weight_lbs")
            weight_str = f" @ {int(weight)} lbs" if weight else ""
            reps = row.get("reps_per_set")
            reps_str = f" \u00d7 {reps} reps" if reps else ""
            items.append(
                ActivityItem(
                    id=f"log-{row['id']}",
                    playerName=player_names.get(row["player_id"], "Unknown"),
                    exercise=row["exercise_name"],
                    details=f"{row['sets_completed']} sets{reps_str}{weight_str} (self-report)",
                    timestamp=row["logged_at"],
                    flagged=False,
                )
            )
    except Exception:
        logger.debug("workout_exercise_logs not available, skipping self-report items")

    # Merge by timestamp descending
    items.sort(key=lambda x: x.timestamp, reverse=True)
    return items[:limit + 5]  # return slightly more than limit for merged feeds


@router.get("/coach/activity-feed", response_model=List[ActivityItem])
def coach_activity_feed(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    _, team_ids, players = _get_coach_context(sb, user_id)

    if not team_ids:
        return []

    player_ids = [p["id"] for p in players]
    player_names = {
        p["id"]: f'{p.get("first_name", "")} {p.get("last_name", "")}'.strip()
        for p in players
    }

    return _build_activity_feed(sb, player_ids, player_names, limit=15)[:20]


@router.get("/coach/due-workouts", response_model=List[DueWorkout])
def coach_due_workouts(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    teams, team_ids, players = _get_coach_context(sb, user_id)

    if not team_ids:
        return []

    now = datetime.now(timezone.utc)
    week_ago_iso = (now - timedelta(days=7)).isoformat()
    two_weeks_ahead_iso = (now + timedelta(days=14)).isoformat()
    now_iso = now.isoformat()

    pbt = _players_by_team(players)
    team_names = {t["id"]: t["name"] for t in teams}

    # Assignments: overdue (last 7 days) + upcoming (next 14 days)
    _due_resp = (
        sb.table("workout_assignments")
        .select(
            "id, team_id, template_id, target_type, target_position_group, "
            "due_at, start_at, created_at, workout_templates(name, content)"
        )
        .in_("team_id", team_ids)
        .gte("due_at", week_ago_iso)
        .lte("due_at", two_weeks_ahead_iso)
        .order("due_at")
        .execute()
    )
    assignments = _due_resp.data if _due_resp else []

    if not assignments:
        return []

    junction_map = _fetch_junction_map(sb, assignments)
    completion = _compute_set_completion(
        sb, assignments, players, junction_map, week_ago_iso, two_weeks_ahead_iso
    )

    target_labels = {"team": "Entire Team", "players": "Selected Players"}

    results: List[DueWorkout] = []
    for a in assignments:
        done, total, _tid = completion.get(a["id"], (0, 0, ""))

        template = a.get("workout_templates")
        template_name = (
            template["name"]
            if isinstance(template, dict) and template
            else "Unnamed Workout"
        )

        target_type = a.get("target_type", "team")
        if target_type == "position_group":
            label = (a.get("target_position_group") or "Unknown").capitalize()
        else:
            label = target_labels.get(target_type, "Entire Team")

        results.append(
            DueWorkout(
                id=a["id"],
                templateName=template_name,
                teamName=team_names.get(a["team_id"], "Unknown Team"),
                targetLabel=label,
                dueAt=a.get("due_at", ""),
                completedCount=done,
                totalCount=total,
                overdue=bool(a.get("due_at") and a["due_at"] < now_iso),
            )
        )

    return results


@router.get("/teams/{team_id}/overview", response_model=TeamOverviewDetail)
def team_overview(team_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()
    team = require_team_access(sb, team_id, user_id)

    # Fetch players for this team
    _pl_resp = sb.table("players").select("*").eq("team_id", team_id).execute()
    players = _pl_resp.data if _pl_resp else []
    player_ids = [p["id"] for p in players]
    player_names = {
        p["id"]: f'{p.get("first_name", "")} {p.get("last_name", "")}'.strip()
        for p in players
    }

    total_players = len(players)
    active_players = sum(1 for p in players if p.get("linked_user_id"))

    now = datetime.now(timezone.utc)
    monday, _ = _week_bounds()
    week_ago = (now - timedelta(days=7)).isoformat()
    two_weeks_ago = (now - timedelta(days=14)).isoformat()
    now_iso = now.isoformat()

    # Assignments created this week for this team
    _atw_resp = (
        sb.table("workout_assignments")
        .select("id")
        .eq("team_id", team_id)
        .gte("created_at", monday)
        .execute()
    )
    assigned_this_week = len(_atw_resp.data if _atw_resp else [])

    # Compliance (last 14 days)
    per_team, _ = _compute_compliance(
        sb, [team_id], players, two_weeks_ago, now_iso
    )
    compliance_pct = per_team.get(team_id, 0)

    # Flagged VBT sets (last 7 days)
    flagged_count = 0
    if player_ids:
        _fc2_resp = (
            sb.table("vbt_set_summaries")
            .select("id")
            .in_("player_id", player_ids)
            .eq("flagged", True)
            .gte("created_at", week_ago)
            .execute()
        )
        flagged_count = len(_fc2_resp.data if _fc2_resp else [])

    # Workouts due this week
    next_monday = (
        now.replace(hour=0, minute=0, second=0, microsecond=0)
        - timedelta(days=now.weekday())
        + timedelta(days=7)
    ).isoformat()
    _wtw_resp = (
        sb.table("workout_assignments")
        .select("id")
        .eq("team_id", team_id)
        .gte("due_at", monday)
        .lt("due_at", next_monday)
        .execute()
    )
    workouts_this_week = len(_wtw_resp.data if _wtw_resp else [])

    stats = [
        StatCard(
            label="Active Players",
            value=active_players,
            subtext=f"of {total_players} total",
            color="blue",
        ),
        StatCard(
            label="Assigned This Week",
            value=assigned_this_week,
            subtext="this team",
            color="blue",
        ),
        StatCard(
            label="Compliance Rate",
            value=f"{compliance_pct}%",
            subtext="last 14 days",
            color="green" if compliance_pct >= 70 else "yellow" if compliance_pct >= 50 else "red",
        ),
        StatCard(
            label="Flagged Sessions",
            value=flagged_count,
            subtext="need form review" if flagged_count else "last 7 days",
            color="red" if flagged_count else "blue",
        ),
    ]

    activity = _build_activity_feed(sb, player_ids, player_names, limit=15)[:15]

    team_overview_obj = TeamOverview(
        id=team["id"],
        name=team["name"],
        sport=team["sport"],
        playerCount=total_players,
        activeCount=active_players,
        workoutsThisWeek=workouts_this_week,
        compliancePercent=compliance_pct,
        needsAttention=flagged_count,
        archived=team.get("archived", False),
    )

    return TeamOverviewDetail(
        team=team_overview_obj,
        stats=stats,
        activity=activity,
    )


# ─── Team Dashboard ─────────────────────────────────────────────────────────

_METRIC_COLUMN = {
    "avg_velocity": "avg_velocity",
    "peak_velocity": "peak_velocity",
    "est_1rm": "estimated_1rm",
}

_METRIC_UNIT = {
    "avg_velocity": "m/s",
    "peak_velocity": "m/s",
    "est_1rm": "lbs",
}


@router.get("/teams/{team_id}/leaderboard", response_model=List[LeaderboardEntry])
def team_leaderboard(
    team_id: str,
    exercise: str = Query("Back Squat"),
    metric: str = Query("peak_velocity"),
    position_group: Optional[str] = Query(None),
    user_id: str = Depends(get_current_user),
):
    sb = _require_db()
    col = _METRIC_COLUMN.get(metric, "peak_velocity")
    unit = _METRIC_UNIT.get(metric, "m/s")

    # Fetch set summaries for this team + exercise, joined with player info
    resp = (
        sb.table("vbt_set_summaries")
        .select(
            "player_id, avg_velocity, peak_velocity, estimated_1rm, created_at, "
            "vbt_raw_sets!inner(team_id), "
            "players!inner(first_name, last_name, jersey_number, position_group)"
        )
        .eq("vbt_raw_sets.team_id", team_id)
        .eq("exercise", exercise)
        .order("created_at", desc=True)
        .execute()
    )

    if not resp or not resp.data:
        return []

    # Find best set per player (highest value for selected metric)
    best: Dict[str, dict] = {}
    for row in resp.data:
        pid = row["player_id"]
        val = row.get(col)
        if val is None:
            continue

        player = row.get("players", {})

        # Apply position filter early
        pg = (player.get("position_group") or "skill").capitalize()
        if position_group and position_group != "all" and pg != position_group:
            continue

        if pid not in best or val > best[pid]["value"]:
            best[pid] = {
                "playerId": pid,
                "playerName": f'{player.get("first_name", "")} {player.get("last_name", "")}'.strip(),
                "jerseyNumber": player.get("jersey_number") or 0,
                "positionGroup": pg,
                "value": round(float(val), 2),
                "unit": unit,
                "date": row.get("created_at", "")[:10],
            }

    # Sort descending by value, assign ranks
    ranked = sorted(best.values(), key=lambda x: x["value"], reverse=True)
    entries = [
        LeaderboardEntry(rank=i + 1, **row)
        for i, row in enumerate(ranked)
    ]

    return entries


@router.get("/teams/{team_id}/live-activity", response_model=List[LivePlayerActivity])
def team_live_activity(team_id: str, user_id: str = Depends(get_current_user)):
    now = datetime.now(timezone.utc)
    return [
        LivePlayerActivity(playerId="p2", playerName="Marcus Williams", jerseyNumber=54, positionGroup="Power", exercise="Back Squat", weight=275, avgVelocity=0.68, peakVelocity=0.78, repCount=4, totalReps=5, startedAt=(now - timedelta(seconds=120)).isoformat()),
        LivePlayerActivity(playerId="p4", playerName="Jaylen Carter", jerseyNumber=7, positionGroup="Skill", exercise="Power Clean", weight=205, avgVelocity=1.05, peakVelocity=1.18, repCount=2, totalReps=3, startedAt=(now - timedelta(seconds=90)).isoformat()),
        LivePlayerActivity(playerId="p3", playerName="Chris Johnson", jerseyNumber=11, positionGroup="Combo", exercise="Bench Press", weight=195, avgVelocity=0.52, peakVelocity=0.61, repCount=5, totalReps=6, startedAt=(now - timedelta(seconds=200)).isoformat()),
        LivePlayerActivity(playerId="p5", playerName="Devon Mitchell", jerseyNumber=34, positionGroup="Power", exercise="Hang Clean", weight=185, avgVelocity=1.12, peakVelocity=1.25, repCount=3, totalReps=3, startedAt=(now - timedelta(seconds=60)).isoformat()),
        LivePlayerActivity(playerId="p6", playerName="Tyler Brooks", jerseyNumber=88, positionGroup="Combo", exercise="Front Squat", weight=225, avgVelocity=0.61, peakVelocity=0.72, repCount=2, totalReps=5, startedAt=(now - timedelta(seconds=45)).isoformat()),
        LivePlayerActivity(playerId="p8", playerName="Malik Robinson", jerseyNumber=45, positionGroup="Power", exercise="Back Squat", weight=295, avgVelocity=0.55, peakVelocity=0.65, repCount=3, totalReps=5, startedAt=(now - timedelta(seconds=180)).isoformat()),
    ]


# ─── Player Dashboard ───────────────────────────────────────────────────────

@router.get("/players/{player_id}/dashboard/today-workout", response_model=Optional[TodayWorkout])
def player_today_workout(player_id: str, user_id: str = Depends(get_current_user)):
    return TodayWorkout(
        id="tw-1",
        name="Week 6 — Heavy Squat Day",
        dueAt=datetime.now(timezone.utc).isoformat(),
        exercises=[
            TodayExercise(name="Back Squat", setGroups=[
                TodaySetGroup(sets=3, reps=5, targetWeight=250, percentOfMax=80, completedSets=3),
                TodaySetGroup(sets=2, reps=3, targetWeight=285, percentOfMax=90, completedSets=0),
            ]),
            TodayExercise(name="Front Squat", setGroups=[
                TodaySetGroup(sets=3, reps=6, targetWeight=160, percentOfMax=65, completedSets=0),
            ]),
            TodayExercise(name="Romanian Deadlift", setGroups=[
                TodaySetGroup(sets=4, reps=8, targetWeight=185, completedSets=0),
            ]),
            TodayExercise(name="Walking Lunges", setGroups=[
                TodaySetGroup(sets=3, reps=12, targetWeight=95, completedSets=0),
            ]),
        ],
    )


@router.get("/players/{player_id}/dashboard/prs", response_model=List[PersonalRecord])
def player_prs(player_id: str, user_id: str = Depends(get_current_user)):
    return [
        PersonalRecord(exercise="Back Squat", weight=315, unit="lbs", peakVelocity=0.42, date="2026-01-28"),
        PersonalRecord(exercise="Bench Press", weight=225, unit="lbs", peakVelocity=0.38, date="2026-02-03"),
        PersonalRecord(exercise="Power Clean", weight=225, unit="lbs", peakVelocity=1.18, date="2026-01-15"),
        PersonalRecord(exercise="Hang Clean", weight=205, unit="lbs", peakVelocity=1.22, date="2026-02-06"),
        PersonalRecord(exercise="Deadlift", weight=385, unit="lbs", peakVelocity=0.35, date="2026-01-22"),
        PersonalRecord(exercise="Front Squat", weight=245, unit="lbs", peakVelocity=0.48, date="2026-02-01"),
    ]


@router.get("/players/{player_id}/dashboard/recent-sessions", response_model=List[WorkoutSession])
def player_recent_sessions(player_id: str, user_id: str = Depends(get_current_user)):
    return [
        WorkoutSession(id="s1", date="2026-02-11", exercise="Back Squat", setsCompleted=5, totalSets=5, repsPerSet=5, avgVelocity=0.65, peakVelocity=0.78, weight=275),
        WorkoutSession(id="s2", date="2026-02-10", exercise="Bench Press", setsCompleted=4, totalSets=4, repsPerSet=6, avgVelocity=0.48, peakVelocity=0.56, weight=195),
        WorkoutSession(id="s3", date="2026-02-08", exercise="Power Clean", setsCompleted=4, totalSets=4, repsPerSet=3, avgVelocity=1.02, peakVelocity=1.18, weight=205),
        WorkoutSession(id="s4", date="2026-02-07", exercise="Back Squat", setsCompleted=5, totalSets=5, repsPerSet=3, avgVelocity=0.58, peakVelocity=0.71, weight=295),
        WorkoutSession(id="s5", date="2026-02-05", exercise="Hang Clean", setsCompleted=5, totalSets=5, repsPerSet=3, avgVelocity=1.08, peakVelocity=1.22, weight=185),
        WorkoutSession(id="s6", date="2026-02-04", exercise="Front Squat", setsCompleted=3, totalSets=4, repsPerSet=6, avgVelocity=0.55, peakVelocity=0.64, weight=215),
        WorkoutSession(id="s7", date="2026-02-03", exercise="Bench Press", setsCompleted=5, totalSets=5, repsPerSet=5, avgVelocity=0.44, peakVelocity=0.52, weight=205),
        WorkoutSession(id="s8", date="2026-02-01", exercise="Deadlift", setsCompleted=4, totalSets=4, repsPerSet=3, avgVelocity=0.38, peakVelocity=0.45, weight=365),
    ]


@router.get("/players/{player_id}/dashboard/velocity-trends", response_model=Dict[str, List[TrendPoint]])
def player_velocity_trends(player_id: str, user_id: str = Depends(get_current_user)):
    return {
        "Back Squat": [
            TrendPoint(date="2025-12-15", avgVelocity=0.58, estimatedMax=285),
            TrendPoint(date="2025-12-22", avgVelocity=0.60, estimatedMax=290),
            TrendPoint(date="2025-12-29", avgVelocity=0.59, estimatedMax=288),
            TrendPoint(date="2026-01-05", avgVelocity=0.62, estimatedMax=295),
            TrendPoint(date="2026-01-12", avgVelocity=0.61, estimatedMax=293),
            TrendPoint(date="2026-01-19", avgVelocity=0.64, estimatedMax=300),
            TrendPoint(date="2026-01-26", avgVelocity=0.63, estimatedMax=298),
            TrendPoint(date="2026-02-02", avgVelocity=0.66, estimatedMax=308),
            TrendPoint(date="2026-02-09", avgVelocity=0.65, estimatedMax=305),
        ],
        "Bench Press": [
            TrendPoint(date="2025-12-15", avgVelocity=0.40, estimatedMax=205),
            TrendPoint(date="2025-12-22", avgVelocity=0.41, estimatedMax=208),
            TrendPoint(date="2025-12-29", avgVelocity=0.42, estimatedMax=210),
            TrendPoint(date="2026-01-05", avgVelocity=0.41, estimatedMax=208),
            TrendPoint(date="2026-01-12", avgVelocity=0.43, estimatedMax=213),
            TrendPoint(date="2026-01-19", avgVelocity=0.44, estimatedMax=215),
            TrendPoint(date="2026-01-26", avgVelocity=0.44, estimatedMax=218),
            TrendPoint(date="2026-02-02", avgVelocity=0.46, estimatedMax=222),
            TrendPoint(date="2026-02-09", avgVelocity=0.48, estimatedMax=225),
        ],
        "Power Clean": [
            TrendPoint(date="2025-12-15", avgVelocity=0.92, estimatedMax=195),
            TrendPoint(date="2025-12-22", avgVelocity=0.95, estimatedMax=198),
            TrendPoint(date="2025-12-29", avgVelocity=0.94, estimatedMax=197),
            TrendPoint(date="2026-01-05", avgVelocity=0.97, estimatedMax=202),
            TrendPoint(date="2026-01-12", avgVelocity=0.98, estimatedMax=205),
            TrendPoint(date="2026-01-19", avgVelocity=1.00, estimatedMax=210),
            TrendPoint(date="2026-01-26", avgVelocity=1.01, estimatedMax=212),
            TrendPoint(date="2026-02-02", avgVelocity=1.02, estimatedMax=215),
            TrendPoint(date="2026-02-09", avgVelocity=1.05, estimatedMax=220),
        ],
    }


@router.get("/players/{player_id}/dashboard/position-comparison", response_model=List[PositionComparison])
def player_position_comparison(player_id: str, user_id: str = Depends(get_current_user)):
    return [
        PositionComparison(exercise="Back Squat", playerAvgVelocity=0.65, groupAvgVelocity=0.58, percentile=78, positionGroup="Power"),
        PositionComparison(exercise="Bench Press", playerAvgVelocity=0.48, groupAvgVelocity=0.45, percentile=65, positionGroup="Power"),
        PositionComparison(exercise="Power Clean", playerAvgVelocity=1.02, groupAvgVelocity=0.94, percentile=82, positionGroup="Power"),
        PositionComparison(exercise="Hang Clean", playerAvgVelocity=1.08, groupAvgVelocity=1.01, percentile=71, positionGroup="Power"),
    ]
