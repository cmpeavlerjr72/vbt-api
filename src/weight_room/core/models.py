"""Pydantic models mirroring the TypeScript types in the frontend."""
from __future__ import annotations

from datetime import datetime
from typing import Any, Dict, List, Literal, Optional

from pydantic import BaseModel


# ── Enums / Literals ─────────────────────────────────────────────────────────

PositionGroup = Literal["skill", "combo", "power"]
TargetType = Literal["team", "position_group", "players"]


# ── Profiles ─────────────────────────────────────────────────────────────────

class ProfileOut(BaseModel):
    id: str
    email: Optional[str] = None
    display_name: Optional[str] = None
    role: str = "coach"
    created_at: str


class ProfileUpdate(BaseModel):
    display_name: Optional[str] = None
    role: Optional[str] = None


# ── Teams ────────────────────────────────────────────────────────────────────

class TeamOut(BaseModel):
    id: str
    coach_id: str
    name: str
    sport: str
    dashboard_config: Dict[str, Any] = {}
    created_at: str


class TeamCreate(BaseModel):
    name: str
    sport: str = "football"


class TeamUpdate(BaseModel):
    name: Optional[str] = None
    sport: Optional[str] = None
    dashboard_config: Optional[Dict[str, Any]] = None


# ── Players ──────────────────────────────────────────────────────────────────

class PlayerOut(BaseModel):
    id: str
    team_id: str
    first_name: str
    last_name: str
    jersey_number: Optional[int] = None
    position_group: str
    rfid_tag_id: Optional[str] = None
    invite_code: Optional[str] = None
    linked_user_id: Optional[str] = None
    linked_at: Optional[str] = None
    created_at: str


class PlayerCreate(BaseModel):
    first_name: str = ""
    last_name: str = ""
    jersey_number: Optional[int] = None
    position_group: PositionGroup = "skill"


class PlayerUpdate(BaseModel):
    first_name: Optional[str] = None
    last_name: Optional[str] = None
    jersey_number: Optional[int] = None
    position_group: Optional[PositionGroup] = None


class ClaimInviteRequest(BaseModel):
    invite_code: str


class PlayerMeOut(BaseModel):
    id: str
    team_id: str
    first_name: str
    last_name: str
    linked_user_id: Optional[str] = None
    linked_at: Optional[str] = None
    teams: Optional[Dict[str, Any]] = None


# ── Player Maxes ─────────────────────────────────────────────────────────────

class PlayerMaxOut(BaseModel):
    id: str
    player_id: str
    exercise: str
    weight: float
    tested_at: str
    created_at: str


class PlayerMaxUpsert(BaseModel):
    exercise: str
    weight: float


# ── RFID ─────────────────────────────────────────────────────────────────────

class RfidTagOut(BaseModel):
    id: str
    uid: str
    team_id: str
    assigned_player_id: Optional[str] = None


class RfidTagCreate(BaseModel):
    uid: str
    team_id: str


class RfidTagAssign(BaseModel):
    player_id: str


class ScanEventCreate(BaseModel):
    team_id: str
    uid: str
    device_id: Optional[str] = None


# ── Workout Templates ────────────────────────────────────────────────────────

class SetGroup(BaseModel):
    sets: int
    reps: int
    percentOfMax: Optional[float] = None
    fixedWeight: Optional[float] = None


class TemplateExercise(BaseModel):
    exerciseName: str
    setGroups: List[SetGroup]
    notes: Optional[str] = None


class WorkoutContent(BaseModel):
    version: int = 2
    exercises: List[TemplateExercise] = []


class WorkoutTemplateOut(BaseModel):
    id: str
    coach_id: str
    sport: str
    name: str
    description: Optional[str] = None
    content: Dict[str, Any]
    created_at: str


class WorkoutTemplateCreate(BaseModel):
    name: str
    description: Optional[str] = None
    sport: str = "football"
    content: Optional[Dict[str, Any]] = None


class WorkoutTemplateUpdate(BaseModel):
    name: Optional[str] = None
    description: Optional[str] = None
    content: Optional[Dict[str, Any]] = None


# ── Workout Assignments ──────────────────────────────────────────────────────

class WorkoutAssignmentOut(BaseModel):
    id: str
    team_id: str
    template_id: str
    target_type: str
    target_position_group: Optional[str] = None
    start_at: Optional[str] = None
    due_at: Optional[str] = None
    status: str
    notes: Optional[str] = None
    created_at: str


class WorkoutAssignmentCreate(BaseModel):
    team_id: str
    template_id: str
    target_type: TargetType = "team"
    target_position_group: Optional[PositionGroup] = None
    player_ids: Optional[List[str]] = None
    start_at: Optional[str] = None
    due_at: Optional[str] = None
    notes: Optional[str] = None


# ── VBT Data ─────────────────────────────────────────────────────────────────

class VbtSetSummaryOut(BaseModel):
    id: str
    raw_set_id: str
    player_id: str
    exercise: str
    rep_count: int
    avg_velocity: float
    peak_velocity: float
    velocity_loss: Optional[float] = None
    estimated_1rm: Optional[float] = None
    flagged: bool
    flag_reason: Optional[str] = None
    created_at: str


class VbtRepOut(BaseModel):
    id: str
    raw_set_id: str
    player_id: str
    exercise: str
    rep_number: int
    mean_velocity: float
    peak_velocity: float
    eccentric_duration: Optional[float] = None
    concentric_duration: Optional[float] = None
    rom_meters: Optional[float] = None
    time_to_peak_vel: Optional[float] = None
    velocity_loss_pct: Optional[float] = None
    bar_path_deviation: Optional[float] = None
    flagged: bool
    flag_reason: Optional[str] = None
    samples: list = []
    created_at: str


# ── Dashboard Models ─────────────────────────────────────────────────────────

class StatCard(BaseModel):
    label: str
    value: Any
    subtext: Optional[str] = None
    trend: Optional[Literal["up", "down", "flat"]] = None
    color: Optional[Literal["green", "yellow", "red", "blue"]] = None


class TeamOverview(BaseModel):
    id: str
    name: str
    sport: str
    playerCount: int
    activeCount: int
    compliancePercent: int
    needsAttention: int
    workoutsThisWeek: int


class ActivityItem(BaseModel):
    id: str
    playerName: str
    exercise: str
    details: str
    timestamp: str
    flagged: bool
    flagReason: Optional[str] = None


class DueWorkout(BaseModel):
    id: str
    templateName: str
    teamName: str
    targetLabel: str
    dueAt: str
    completedCount: int
    totalCount: int
    overdue: bool


class PersonalRecord(BaseModel):
    exercise: str
    weight: float
    unit: str
    peakVelocity: float
    date: str


class WorkoutSession(BaseModel):
    id: str
    date: str
    exercise: str
    setsCompleted: int
    totalSets: int
    repsPerSet: int
    avgVelocity: float
    peakVelocity: float
    weight: float


class TrendPoint(BaseModel):
    date: str
    avgVelocity: float
    estimatedMax: float


class TodaySetGroup(BaseModel):
    sets: int
    reps: int
    targetWeight: Optional[float] = None
    percentOfMax: Optional[float] = None
    completedSets: int


class TodayExercise(BaseModel):
    name: str
    setGroups: List[TodaySetGroup]
    notes: Optional[str] = None


class TodayWorkout(BaseModel):
    id: str
    name: str
    dueAt: str
    exercises: List[TodayExercise]


class PositionComparison(BaseModel):
    exercise: str
    playerAvgVelocity: float
    groupAvgVelocity: float
    percentile: int
    positionGroup: str


class LeaderboardEntry(BaseModel):
    rank: int
    playerId: str
    playerName: str
    jerseyNumber: int
    positionGroup: str
    value: float
    unit: str
    date: str


class LivePlayerActivity(BaseModel):
    playerId: str
    playerName: str
    jerseyNumber: int
    positionGroup: str
    exercise: str
    weight: float
    avgVelocity: float
    peakVelocity: float
    repCount: int
    totalReps: int
    startedAt: str


# ── Device (ESP32) Models ───────────────────────────────────────────────

class DevicePlayerOut(BaseModel):
    id: str
    first_name: str
    last_name: str
    jersey_number: Optional[int] = None


class VelSample(BaseModel):
    t: int
    v: float


class DeviceRepIn(BaseModel):
    rep_number: int
    mean_velocity: float
    peak_velocity: float
    rom_meters: Optional[float] = None
    concentric_duration: Optional[float] = None
    eccentric_duration: Optional[float] = None
    samples: List[VelSample] = []


class DeviceSetIn(BaseModel):
    team_id: str
    player_id: str
    exercise: str
    device_id: str
    reps: List[DeviceRepIn]


class DeviceSetOut(BaseModel):
    set_id: str
    reps_created: int
