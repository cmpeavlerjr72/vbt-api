"""Supabase client lazy singleton.

All operations are wrapped so that Supabase unavailability never breaks the app.
"""
from __future__ import annotations

import logging
from typing import Optional

log = logging.getLogger(__name__)

_supabase_client = None
_supabase_checked = False


def get_supabase():
    """Lazy singleton.  Returns ``supabase.Client`` or ``None`` if unavailable."""
    global _supabase_client, _supabase_checked
    if _supabase_checked:
        return _supabase_client
    _supabase_checked = True
    try:
        from weight_room.config import settings

        if not settings.supabase_url or not settings.supabase_service_key:
            log.info("Supabase not configured, running without database")
            return None
        from supabase import create_client

        _supabase_client = create_client(
            settings.supabase_url, settings.supabase_service_key
        )
        log.info("Supabase connected: %s", settings.supabase_url)
    except Exception as exc:
        log.warning("Supabase unavailable (%s), running without database", exc)
        _supabase_client = None
    return _supabase_client
