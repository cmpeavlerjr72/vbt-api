"""Centralized settings for the weight-room backend."""
from __future__ import annotations

from pathlib import Path

from pydantic_settings import BaseSettings

_ENV_FILE = Path(__file__).resolve().parents[2] / ".env"


class Settings(BaseSettings):
    model_config = {"env_prefix": "WEIGHT_ROOM_", "env_file": str(_ENV_FILE)}

    # Supabase — empty strings mean disabled (graceful fallback)
    supabase_url: str = ""
    supabase_service_key: str = ""
    supabase_jwt_secret: str = ""

    # Gunicorn
    gunicorn_workers: int = 2


settings = Settings()
