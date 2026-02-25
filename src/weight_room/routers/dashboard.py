"""Dashboard endpoints — all backed by real DB queries."""
from __future__ import annotations

import logging
from collections import defaultdict
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
    TrendPoint,
    WorkoutSession,
)
from weight_room.db import get_supabase
from weight_room.routers.workouts import _parse_exercises, _resolve_weight

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


def _build_weight_resolver(sb, player_id: str, player_maxes: Dict[str, float]):
    """Build a callable that resolves weight for a (exercise, created_at) pair.

    Looks up active assignments for the player's team, parses their templates,
    and matches by exercise name + time window to compute:
        target_weight = percentOfMax / 100 * player_max  (rounded to nearest 5 lbs)
    Falls back to 0 if no matching assignment found.
    """
    # Get player's team
    _p_resp = sb.table("players").select("team_id").eq("id", player_id).execute()
    if not _p_resp or not _p_resp.data:
        return lambda exercise, created_at: 0

    team_id = _p_resp.data[0]["team_id"]

    # Fetch assignments with templates for this team
    _a_resp = (
        sb.table("workout_assignments")
        .select("id, start_at, due_at, created_at, workout_templates(content)")
        .eq("team_id", team_id)
        .execute()
    )
    assignments = _a_resp.data if _a_resp else []

    # Build lookup: list of (exercise, window_start, window_end, percentOfMax, fixedWeight)
    entries: list[tuple] = []
    for a in assignments:
        tmpl = a.get("workout_templates")
        content = tmpl.get("content", {}) if isinstance(tmpl, dict) and tmpl else {}
        parsed = _parse_exercises(content)
        w_start, w_end = _assignment_vbt_window(
            a,
            a.get("created_at", ""),
            (a.get("due_at") or a.get("created_at", ""))[:10] + "T23:59:59+00:00",
        )
        for ex in parsed:
            for sg in ex.get("set_groups", []):
                entries.append((
                    ex["exercise_name"],
                    w_start,
                    w_end,
                    sg.get("percentOfMax"),
                    sg.get("fixedWeight"),
                ))

    def resolve(exercise: str, created_at: str) -> float:
        for ex_name, w_start, w_end, pct, fixed in entries:
            if ex_name == exercise and w_start <= created_at <= w_end:
                w = _resolve_weight(pct, fixed, player_maxes.get(exercise))
                if w is not None:
                    return w
        return 0

    return resolve


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
    sb = _require_db()

    five_min_ago = (datetime.now(timezone.utc) - timedelta(minutes=5)).isoformat()

    # Recent raw sets with player info
    resp = (
        sb.table("vbt_raw_sets")
        .select(
            "id, player_id, exercise, created_at, "
            "players!inner(first_name, last_name, jersey_number, position_group)"
        )
        .eq("team_id", team_id)
        .gte("created_at", five_min_ago)
        .order("created_at", desc=True)
        .execute()
    )
    raw_rows = resp.data if resp else []
    if not raw_rows:
        return []

    # Dedupe to most recent set per player
    seen_players: Dict[str, dict] = {}
    for row in raw_rows:
        pid = row["player_id"]
        if pid not in seen_players:
            seen_players[pid] = row

    raw_set_ids = [row["id"] for row in seen_players.values()]

    # Get set summaries for those raw sets
    summary_resp = (
        sb.table("vbt_set_summaries")
        .select("raw_set_id, avg_velocity, peak_velocity, rep_count")
        .in_("raw_set_id", raw_set_ids)
        .execute()
    )
    summaries = {
        s["raw_set_id"]: s for s in (summary_resp.data if summary_resp else [])
    }

    # Get player maxes for weight resolution
    player_ids = list(seen_players.keys())
    maxes_resp = (
        sb.table("player_maxes")
        .select("player_id, exercise, weight")
        .in_("player_id", player_ids)
        .execute()
    )
    # Build per-player maxes: {player_id: {exercise: weight}}
    per_player_maxes: Dict[str, Dict[str, float]] = {}
    for m in (maxes_resp.data if maxes_resp else []):
        per_player_maxes.setdefault(m["player_id"], {})[m["exercise"]] = m["weight"]

    # Build weight resolvers per player
    resolvers: Dict[str, object] = {}
    for pid in player_ids:
        resolvers[pid] = _build_weight_resolver(sb, pid, per_player_maxes.get(pid, {}))

    results = []
    for pid, row in seen_players.items():
        player = row.get("players", {})
        summary = summaries.get(row["id"], {})
        resolve = resolvers.get(pid, lambda ex, ts: 0)

        results.append(LivePlayerActivity(
            playerId=pid,
            playerName=f'{player.get("first_name", "")} {player.get("last_name", "")}'.strip(),
            jerseyNumber=player.get("jersey_number") or 0,
            positionGroup=(player.get("position_group") or "skill").capitalize(),
            exercise=row["exercise"],
            weight=resolve(row["exercise"], row["created_at"]),
            avgVelocity=round(float(summary.get("avg_velocity", 0)), 2),
            peakVelocity=round(float(summary.get("peak_velocity", 0)), 2),
            repCount=summary.get("rep_count", 0),
            totalReps=summary.get("rep_count", 0),
            startedAt=row["created_at"],
        ))

    return results


# ─── Player Dashboard ───────────────────────────────────────────────────────


def _get_player_maxes(sb, player_id: str) -> Dict[str, float]:
    """Return {exercise: weight} for a player's current maxes."""
    resp = (
        sb.table("player_maxes")
        .select("exercise, weight")
        .eq("player_id", player_id)
        .execute()
    )
    return {m["exercise"]: m["weight"] for m in (resp.data if resp else [])}


@router.get("/players/{player_id}/dashboard/prs", response_model=List[PersonalRecord])
def player_prs(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()

    resp = (
        sb.table("vbt_set_summaries")
        .select("exercise, peak_velocity, created_at")
        .eq("player_id", player_id)
        .order("peak_velocity", desc=True)
        .execute()
    )
    rows = resp.data if resp else []
    if not rows:
        return []

    # Dedupe to highest peak_velocity per exercise
    best: Dict[str, dict] = {}
    for row in rows:
        ex = row["exercise"]
        if ex not in best:
            best[ex] = row

    maxes = _get_player_maxes(sb, player_id)
    resolve = _build_weight_resolver(sb, player_id, maxes)

    return [
        PersonalRecord(
            exercise=ex,
            weight=resolve(ex, row["created_at"]),
            unit="lbs",
            peakVelocity=round(float(row["peak_velocity"]), 2),
            date=row["created_at"][:10],
        )
        for ex, row in best.items()
    ]


@router.get("/players/{player_id}/dashboard/recent-sessions", response_model=List[WorkoutSession])
def player_recent_sessions(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()

    resp = (
        sb.table("vbt_set_summaries")
        .select("id, exercise, rep_count, avg_velocity, peak_velocity, created_at")
        .eq("player_id", player_id)
        .order("created_at", desc=True)
        .limit(20)
        .execute()
    )
    rows = resp.data if resp else []
    if not rows:
        return []

    maxes = _get_player_maxes(sb, player_id)
    resolve = _build_weight_resolver(sb, player_id, maxes)

    return [
        WorkoutSession(
            id=row["id"],
            date=row["created_at"][:10],
            exercise=row["exercise"],
            setsCompleted=1,
            totalSets=1,
            repsPerSet=row["rep_count"],
            avgVelocity=round(float(row["avg_velocity"]), 2),
            peakVelocity=round(float(row["peak_velocity"]), 2),
            weight=resolve(row["exercise"], row["created_at"]),
        )
        for row in rows
    ]


@router.get("/players/{player_id}/dashboard/velocity-trends", response_model=Dict[str, List[TrendPoint]])
def player_velocity_trends(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()

    resp = (
        sb.table("vbt_set_summaries")
        .select("exercise, avg_velocity, estimated_1rm, created_at")
        .eq("player_id", player_id)
        .order("created_at", desc=False)
        .execute()
    )
    rows = resp.data if resp else []
    if not rows:
        return {}

    # Group by exercise -> date -> rows
    exercise_dates: Dict[str, Dict[str, list]] = defaultdict(lambda: defaultdict(list))
    for row in rows:
        exercise_dates[row["exercise"]][row["created_at"][:10]].append(row)

    result: Dict[str, List[TrendPoint]] = {}
    for ex, dates in exercise_dates.items():
        points = []
        for date in sorted(dates.keys()):
            day_rows = dates[date]
            avg_vel = sum(float(r["avg_velocity"]) for r in day_rows) / len(day_rows)
            est_1rms = [
                float(r["estimated_1rm"])
                for r in day_rows
                if r.get("estimated_1rm") is not None
            ]
            avg_1rm = sum(est_1rms) / len(est_1rms) if est_1rms else 0
            points.append(TrendPoint(
                date=date,
                avgVelocity=round(avg_vel, 2),
                estimatedMax=round(avg_1rm),
            ))
        result[ex] = points

    return result


@router.get("/players/{player_id}/dashboard/position-comparison", response_model=List[PositionComparison])
def player_position_comparison(player_id: str, user_id: str = Depends(get_current_user)):
    sb = _require_db()

    # Look up player's team and position group
    player_resp = (
        sb.table("players")
        .select("team_id, position_group")
        .eq("id", player_id)
        .single()
        .execute()
    )
    if not player_resp or not player_resp.data:
        return []

    player = player_resp.data
    position_group = player.get("position_group")
    team_id = player["team_id"]

    if not position_group:
        return []

    # Find all teammates with same position group
    teammates_resp = (
        sb.table("players")
        .select("id")
        .eq("team_id", team_id)
        .eq("position_group", position_group)
        .execute()
    )
    teammate_ids = [t["id"] for t in (teammates_resp.data if teammates_resp else [])]
    if not teammate_ids:
        return []

    # Get all VBT data for these teammates
    vbt_resp = (
        sb.table("vbt_set_summaries")
        .select("player_id, exercise, avg_velocity")
        .in_("player_id", teammate_ids)
        .execute()
    )
    vbt_rows = vbt_resp.data if vbt_resp else []
    if not vbt_rows:
        return []

    # Group by exercise -> player -> velocities
    exercise_player_vels: Dict[str, Dict[str, list]] = defaultdict(lambda: defaultdict(list))
    for row in vbt_rows:
        exercise_player_vels[row["exercise"]][row["player_id"]].append(
            float(row["avg_velocity"])
        )

    results = []
    for exercise, player_vels in exercise_player_vels.items():
        if player_id not in player_vels:
            continue

        player_avg = sum(player_vels[player_id]) / len(player_vels[player_id])

        # Group average across all teammates
        all_vels = [v for vels in player_vels.values() for v in vels]
        group_avg = sum(all_vels) / len(all_vels) if all_vels else 0

        # Percentile: fraction of teammates with lower average
        teammate_avgs = [sum(vels) / len(vels) for vels in player_vels.values()]
        if len(teammate_avgs) <= 1:
            percentile = 50
        else:
            below = sum(1 for avg in teammate_avgs if avg < player_avg)
            percentile = round(below / len(teammate_avgs) * 100)

        results.append(PositionComparison(
            exercise=exercise,
            playerAvgVelocity=round(player_avg, 2),
            groupAvgVelocity=round(group_avg, 2),
            percentile=percentile,
            positionGroup=position_group.capitalize(),
        ))

    return results
