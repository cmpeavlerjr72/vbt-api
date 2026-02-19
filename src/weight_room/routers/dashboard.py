"""Dashboard endpoints.

Returns mock data initially — same shapes the frontend currently hardcodes.
Replace with real queries as data flows in.
"""
from __future__ import annotations

import random
from datetime import datetime, timedelta, timezone
from typing import Dict, List, Optional

from fastapi import APIRouter, Depends, HTTPException, Query

from weight_room.auth import get_current_user
from weight_room.core.models import (
    ActivityItem,
    DueWorkout,
    LeaderboardEntry,
    LivePlayerActivity,
    PersonalRecord,
    PositionComparison,
    StatCard,
    TeamOverview,
    TodayWorkout,
    TodayExercise,
    TodaySetGroup,
    TrendPoint,
    WorkoutSession,
)
from weight_room.db import get_supabase

router = APIRouter(tags=["dashboard"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


# ─── Coach Dashboard ────────────────────────────────────────────────────────

@router.get("/coach/stats", response_model=List[StatCard])
def coach_stats(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = sb.table("teams").select("id").eq("coach_id", user_id).execute()
    team_count = len(resp.data)
    total_players = team_count * 28  # mock avg roster size

    return [
        StatCard(label="Active Players", value=round(total_players * 0.82), subtext=f"of {total_players} total", color="blue"),
        StatCard(label="Assigned This Week", value=12, subtext="across all teams", color="blue"),
        StatCard(label="Compliance Rate", value="78%", subtext="+4% from last week", trend="up", color="green"),
        StatCard(label="Flagged Sessions", value=3, subtext="need form review", color="red"),
    ]


@router.get("/coach/team-overviews", response_model=List[TeamOverview])
def coach_team_overviews(user_id: str = Depends(get_current_user)):
    sb = _require_db()
    resp = sb.table("teams").select("*").eq("coach_id", user_id).execute()

    return [
        TeamOverview(
            id=t["id"],
            name=t["name"],
            sport=t["sport"],
            playerCount=30,
            activeCount=24,
            compliancePercent=round(60 + random.random() * 35),
            needsAttention=random.randint(0, 5),
            workoutsThisWeek=random.randint(2, 5),
        )
        for t in resp.data
    ]


@router.get("/coach/activity-feed", response_model=List[ActivityItem])
def coach_activity_feed(user_id: str = Depends(get_current_user)):
    now = datetime.now(timezone.utc)
    hour = timedelta(hours=1)

    return [
        ActivityItem(id="1", playerName="Marcus Williams", exercise="Back Squat", details="5x5 @ 275 lbs — avg velocity 0.68 m/s", timestamp=(now - hour * 1).isoformat(), flagged=False),
        ActivityItem(id="2", playerName="Jaylen Carter", exercise="Power Clean", details="4x3 @ 205 lbs — avg velocity 1.05 m/s", timestamp=(now - hour * 2).isoformat(), flagged=True, flagReason="Unusual bar path detected on sets 2-3"),
        ActivityItem(id="3", playerName="Chris Johnson", exercise="Bench Press", details="4x6 @ 195 lbs — avg velocity 0.52 m/s", timestamp=(now - hour * 3.5).isoformat(), flagged=False),
        ActivityItem(id="4", playerName="Devon Mitchell", exercise="Hang Clean", details="5x3 @ 185 lbs — avg velocity 1.12 m/s", timestamp=(now - hour * 5).isoformat(), flagged=False),
        ActivityItem(id="5", playerName="Tyler Brooks", exercise="Front Squat", details="4x4 @ 225 lbs — avg velocity 0.61 m/s", timestamp=(now - hour * 6).isoformat(), flagged=True, flagReason="Excessive forward lean on reps 3-4"),
        ActivityItem(id="6", playerName="Andre Davis", exercise="Back Squat", details="5x5 @ 315 lbs — avg velocity 0.55 m/s", timestamp=(now - hour * 8).isoformat(), flagged=False),
        ActivityItem(id="7", playerName="Isaiah Thompson", exercise="Bench Press", details="5x5 @ 225 lbs — avg velocity 0.48 m/s", timestamp=(now - hour * 10).isoformat(), flagged=False),
    ]


@router.get("/coach/due-workouts", response_model=List[DueWorkout])
def coach_due_workouts(user_id: str = Depends(get_current_user)):
    now = datetime.now(timezone.utc)
    day = timedelta(days=1)

    return [
        DueWorkout(id="1", templateName="Week 6 — Heavy Squat Day", teamName="Varsity Football", targetLabel="Entire Team", dueAt=(now + day * 2).isoformat(), completedCount=18, totalCount=30, overdue=False),
        DueWorkout(id="2", templateName="Upper Body Hypertrophy", teamName="Varsity Football", targetLabel="Skill Position", dueAt=(now + day * 1).isoformat(), completedCount=4, totalCount=12, overdue=False),
        DueWorkout(id="3", templateName="Week 5 — Power Clean Complex", teamName="Varsity Football", targetLabel="Power Position", dueAt=(now - day * 1).isoformat(), completedCount=8, totalCount=14, overdue=True),
        DueWorkout(id="4", templateName="Accessory Work — Week 6", teamName="Varsity Football", targetLabel="Entire Team", dueAt=(now + day * 4).isoformat(), completedCount=0, totalCount=30, overdue=False),
    ]


# ─── Team Dashboard ─────────────────────────────────────────────────────────

_MOCK_PLAYERS = [
    {"id": "p1", "name": "Andre Davis", "jersey": 22, "pos": "Power"},
    {"id": "p2", "name": "Marcus Williams", "jersey": 54, "pos": "Power"},
    {"id": "p3", "name": "Chris Johnson", "jersey": 11, "pos": "Combo"},
    {"id": "p4", "name": "Jaylen Carter", "jersey": 7, "pos": "Skill"},
    {"id": "p5", "name": "Devon Mitchell", "jersey": 34, "pos": "Power"},
    {"id": "p6", "name": "Tyler Brooks", "jersey": 88, "pos": "Combo"},
    {"id": "p7", "name": "Isaiah Thompson", "jersey": 3, "pos": "Skill"},
    {"id": "p8", "name": "Malik Robinson", "jersey": 45, "pos": "Power"},
    {"id": "p9", "name": "Caleb Stewart", "jersey": 19, "pos": "Skill"},
    {"id": "p10", "name": "Jordan Wright", "jersey": 66, "pos": "Combo"},
]

_MOCK_RECORDS: Dict[str, dict] = {
    "Back Squat": {"values": [315, 305, 285, 275, 270, 265, 255, 245, 240, 235], "unit": "lbs", "dates": ["2026-01-28", "2026-02-03", "2026-01-22", "2026-01-15", "2026-02-10", "2026-01-30", "2026-02-05", "2026-01-20", "2026-02-08", "2026-01-25"]},
    "Bench Press": {"values": [275, 265, 245, 235, 225, 220, 215, 205, 200, 195], "unit": "lbs", "dates": ["2026-02-01", "2026-01-28", "2026-02-06", "2026-01-18", "2026-02-03", "2026-01-22", "2026-02-09", "2026-01-15", "2026-02-12", "2026-01-25"]},
    "Power Clean": {"values": [265, 255, 245, 235, 225, 220, 215, 205, 200, 195], "unit": "lbs", "dates": ["2026-02-05", "2026-01-30", "2026-02-10", "2026-01-20", "2026-02-03", "2026-01-25", "2026-02-08", "2026-01-18", "2026-02-12", "2026-01-22"]},
    "Hang Clean": {"values": [245, 235, 225, 220, 215, 205, 200, 195, 190, 185], "unit": "lbs", "dates": ["2026-02-06", "2026-01-28", "2026-02-03", "2026-01-20", "2026-02-10", "2026-01-25", "2026-02-08", "2026-01-15", "2026-02-12", "2026-01-22"]},
    "Front Squat": {"values": [275, 265, 255, 245, 240, 235, 225, 220, 215, 210], "unit": "lbs", "dates": ["2026-02-01", "2026-01-30", "2026-02-07", "2026-01-22", "2026-02-05", "2026-01-18", "2026-02-10", "2026-01-28", "2026-02-12", "2026-01-25"]},
}


@router.get("/teams/{team_id}/leaderboard", response_model=List[LeaderboardEntry])
def team_leaderboard(
    team_id: str,
    exercise: str = Query("Back Squat"),
    metric: str = Query("max_weight"),
    position_group: Optional[str] = Query(None),
    user_id: str = Depends(get_current_user),
):
    rec = _MOCK_RECORDS.get(exercise)
    if not rec:
        return []

    entries = [
        LeaderboardEntry(
            rank=i + 1,
            playerId=p["id"],
            playerName=p["name"],
            jerseyNumber=p["jersey"],
            positionGroup=p["pos"],
            value=rec["values"][i],
            unit=rec["unit"],
            date=rec["dates"][i],
        )
        for i, p in enumerate(_MOCK_PLAYERS)
    ]

    if position_group and position_group != "all":
        entries = [e for e in entries if e.positionGroup == position_group]
        for i, e in enumerate(entries):
            e.rank = i + 1

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
