"""Shared team-access helpers for multi-coach support."""
from __future__ import annotations

from fastapi import HTTPException


def require_team_access(sb, team_id: str, user_id: str) -> dict:
    """Return team row if user is owner OR in team_coaches. Raise 404 otherwise."""
    team_resp = sb.table("teams").select("*").eq("id", team_id).maybe_single().execute()
    if not team_resp.data:
        raise HTTPException(status_code=404, detail="Team not found")
    team = team_resp.data
    if team["coach_id"] == user_id:
        return team
    # Check team_coaches junction
    tc = (
        sb.table("team_coaches")
        .select("id")
        .eq("team_id", team_id)
        .eq("coach_id", user_id)
        .maybe_single()
        .execute()
    )
    if tc.data:
        return team
    raise HTTPException(status_code=404, detail="Team not found")


def require_team_owner(sb, team_id: str, user_id: str) -> dict:
    """Return team row if user is the owner (coach_id). Raise 403 otherwise."""
    team_resp = sb.table("teams").select("*").eq("id", team_id).maybe_single().execute()
    if not team_resp.data:
        raise HTTPException(status_code=404, detail="Team not found")
    if team_resp.data["coach_id"] != user_id:
        raise HTTPException(status_code=403, detail="Only the head coach can do this")
    return team_resp.data


def get_all_accessible_teams(
    sb, user_id: str, *, include_archived: bool = False
) -> list[dict]:
    """Return all teams where user is owner OR assistant."""
    # Teams where user is the owner
    q = sb.table("teams").select("*").eq("coach_id", user_id)
    if not include_archived:
        q = q.eq("archived", False)
    owned = q.execute().data

    # Teams where user is an assistant via team_coaches
    tc_rows = (
        sb.table("team_coaches")
        .select("team_id")
        .eq("coach_id", user_id)
        .execute()
        .data
    )
    assistant_ids = [r["team_id"] for r in tc_rows]
    # Remove team IDs already in owned set
    owned_ids = {t["id"] for t in owned}
    extra_ids = [tid for tid in assistant_ids if tid not in owned_ids]

    if extra_ids:
        q2 = sb.table("teams").select("*").in_("id", extra_ids)
        if not include_archived:
            q2 = q2.eq("archived", False)
        extra = q2.execute().data
        owned.extend(extra)

    return owned
