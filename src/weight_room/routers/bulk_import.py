"""Bulk import endpoints for mass-uploading player data from spreadsheets."""
from __future__ import annotations

from typing import List

from fastapi import APIRouter, Depends, HTTPException

from weight_room.auth import get_current_user
from weight_room.core.access import require_team_access
from weight_room.core.models import BulkMaxImportRequest, BulkMaxImportResult
from weight_room.db import get_supabase

router = APIRouter(tags=["bulk-import"])


def _require_db():
    sb = get_supabase()
    if sb is None:
        raise HTTPException(status_code=503, detail="Database unavailable")
    return sb


@router.post(
    "/teams/{team_id}/import/maxes",
    response_model=BulkMaxImportResult,
    status_code=200,
)
def bulk_import_maxes(
    team_id: str,
    body: BulkMaxImportRequest,
    user_id: str = Depends(get_current_user),
):
    """Bulk import player maxes from spreadsheet data.

    - Creates players that don't exist (matched by first + last name on team)
    - Upserts maxes with proper tested_at dates
    - Rows are processed in chronological order so history is preserved
    """
    sb = _require_db()
    require_team_access(sb, team_id, user_id)

    # Load existing players on this team for name matching
    existing_resp = (
        sb.table("players")
        .select("id, first_name, last_name")
        .eq("team_id", team_id)
        .execute()
    )
    # Build lookup: "first_name|last_name" (lowered) -> player_id
    player_lookup: dict[str, str] = {}
    for p in existing_resp.data or []:
        key = f"{p['first_name'].strip().lower()}|{p['last_name'].strip().lower()}"
        player_lookup[key] = p["id"]

    # Sort rows chronologically so earlier tests get archived properly
    sorted_rows = sorted(body.rows, key=lambda r: r.tested_at)

    players_created = 0
    maxes_upserted = 0
    skipped = 0
    errors: list[str] = []

    for row in sorted_rows:
        name_key = f"{row.first_name.strip().lower()}|{row.last_name.strip().lower()}"

        # Create player if not found
        if name_key not in player_lookup:
            try:
                create_resp = (
                    sb.table("players")
                    .insert({
                        "team_id": team_id,
                        "first_name": row.first_name.strip(),
                        "last_name": row.last_name.strip(),
                    })
                    .execute()
                )
                player_lookup[name_key] = create_resp.data[0]["id"]
                players_created += 1
            except Exception as e:
                errors.append(f"Failed to create {row.first_name} {row.last_name}: {e}")
                continue

        player_id = player_lookup[name_key]

        # Skip zero-weight rows (no data recorded)
        if row.weight <= 0:
            skipped += 1
            continue

        try:
            # Archive existing value to history before overwriting
            existing = (
                sb.table("player_maxes")
                .select("*")
                .eq("player_id", player_id)
                .eq("exercise", row.exercise)
                .maybe_single()
                .execute()
            )
            if existing and existing.data:
                sb.table("player_max_history").insert({
                    "player_id": existing.data["player_id"],
                    "exercise": existing.data["exercise"],
                    "weight": existing.data["weight"],
                    "tested_at": existing.data["tested_at"],
                }).execute()

            # Upsert current max
            sb.table("player_maxes").upsert(
                {
                    "player_id": player_id,
                    "exercise": row.exercise,
                    "weight": row.weight,
                    "tested_at": row.tested_at,
                },
                on_conflict="player_id,exercise",
            ).execute()
            maxes_upserted += 1

        except Exception as e:
            errors.append(
                f"Failed max for {row.first_name} {row.last_name} "
                f"({row.exercise}): {e}"
            )

    return BulkMaxImportResult(
        players_created=players_created,
        maxes_upserted=maxes_upserted,
        skipped=skipped,
        errors=errors,
    )
