# Weight Room — Backend API

Python/FastAPI backend for the weight-lifting tracker. Sits between the React frontend and Supabase (PostgreSQL + Auth).

## Running Locally

```bash
pip install -e .
uvicorn weight_room.api:app --reload
# API docs at http://localhost:8000/docs
```

## Project Structure

```
src/weight_room/
├── api.py          # FastAPI app, CORS, router registration, /health
├── auth.py         # JWT verification (JWKS + HS256 fallback)
├── config.py       # Pydantic Settings (WEIGHT_ROOM_ env prefix)
├── db.py           # Supabase client lazy singleton
├── core/
│   └── models.py   # All Pydantic request/response models
└── routers/
    ├── profiles.py   # /profiles/me
    ├── teams.py      # /teams CRUD
    ├── players.py    # /teams/{id}/players, /players/{id}, /players/claim, /players/me
    ├── maxes.py      # /players/{id}/maxes, /teams/{id}/maxes, /maxes/{id}
    ├── workouts.py   # /templates, /teams/{id}/assignments, /assignments
    ├── rfid.py       # /rfid/lookup, /rfid/tags, /scan-events
    ├── vbt.py        # /players/{id}/vbt/*, /teams/{id}/vbt/*
    └── dashboard.py  # /coach/*, /teams/{id}/leaderboard, /players/{id}/dashboard/*
```

## SQL Migrations

Located in `sql/` — run files 01-10 in order in the Supabase SQL Editor.

## Key Patterns

- **Auth**: `get_current_user` / `get_optional_user` FastAPI dependencies extract user_id from JWT
- **DB**: `get_supabase()` returns client or None; routers check with `_require_db()`
- **Service role**: Backend uses service key to bypass RLS
- **Dashboard**: Returns mock data initially, same shapes as frontend hardcodes

## Endpoints Summary

| Group | Endpoints |
|-------|-----------|
| Health | `GET /health` |
| Profiles | `GET/PUT /profiles/me` |
| Teams | `GET/POST /teams`, `GET/PUT /teams/{id}` |
| Players | `GET/POST /teams/{id}/players`, `PUT/DELETE /players/{id}`, `POST /players/claim`, `GET /players/me` |
| Maxes | `GET/POST /players/{id}/maxes`, `GET /teams/{id}/maxes`, `DELETE /maxes/{id}` |
| Templates | `GET/POST /templates`, `PUT/DELETE /templates/{id}` |
| Assignments | `GET /teams/{id}/assignments`, `POST /assignments` |
| RFID | `GET /rfid/lookup`, `POST /rfid/tags`, `PUT /rfid/tags/{id}/assign`, `POST /scan-events` |
| VBT | `GET /players/{id}/vbt/set-summaries`, `GET /vbt/sets/{id}/reps`, `GET /players/{id}/vbt/recent-reps`, `GET /players/{id}/vbt/prs`, `GET /teams/{id}/vbt/set-summaries`, `GET /teams/{id}/vbt/flagged-reps` |
| Dashboard | `GET /coach/stats`, `GET /coach/team-overviews`, `GET /coach/activity-feed`, `GET /coach/due-workouts`, `GET /teams/{id}/leaderboard`, `GET /teams/{id}/live-activity`, `GET /players/{id}/dashboard/*` |
