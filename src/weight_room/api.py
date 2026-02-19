"""FastAPI REST backend for the weight-room tracker."""
from __future__ import annotations

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from weight_room.db import get_supabase
from weight_room.routers import (
    dashboard,
    maxes,
    players,
    profiles,
    rfid,
    teams,
    vbt,
    workouts,
)

app = FastAPI(title="Weight Room", version="0.1.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ---------------------------------------------------------------------------
# Include routers
# ---------------------------------------------------------------------------
app.include_router(profiles.router)
app.include_router(teams.router)
app.include_router(players.router)
app.include_router(maxes.router)
app.include_router(workouts.router)
app.include_router(rfid.router)
app.include_router(vbt.router)
app.include_router(dashboard.router)


# ---------------------------------------------------------------------------
# Health check
# ---------------------------------------------------------------------------
@app.get("/health")
def health():
    supabase_ok = get_supabase() is not None
    return {"status": "ok", "supabase": supabase_ok}
