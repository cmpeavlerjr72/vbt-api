"""Supabase client lazy singleton.

Thread-safe initialisation so concurrent requests during cold start
don't race and return None while the client is still being created.
"""
from __future__ import annotations

import logging
import threading
from typing import Optional

log = logging.getLogger(__name__)

_supabase_client = None
_init_lock = threading.Lock()
_init_done = False


def get_supabase():
    """Lazy singleton.  Returns ``supabase.Client`` or ``None`` if unavailable."""
    global _supabase_client, _init_done
    if _init_done:
        return _supabase_client
    with _init_lock:
        # Double-check inside the lock
        if _init_done:
            return _supabase_client
        try:
            from weight_room.config import settings

            if not settings.supabase_url or not settings.supabase_service_key:
                log.info("Supabase not configured, running without database")
            else:
                from supabase import create_client

                _supabase_client = create_client(
                    settings.supabase_url, settings.supabase_service_key
                )
                log.info("Supabase connected: %s", settings.supabase_url)
        except Exception as exc:
            log.warning("Supabase unavailable (%s), running without database", exc)
            _supabase_client = None
        _init_done = True
    return _supabase_client
