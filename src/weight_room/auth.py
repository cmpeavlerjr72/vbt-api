"""JWT verification: FastAPI dependencies for Supabase Auth.

- ``get_current_user``: requires a valid JWT, returns user_id (UUID str).
- ``get_optional_user``: returns user_id or None (for public endpoints).

Supports both HS256 (legacy) and ES256/JWKS (newer Supabase projects).
"""
from __future__ import annotations

import logging
from typing import Optional

import jwt
from jwt import PyJWKClient
from fastapi import Depends, HTTPException, Request

from weight_room.config import settings

log = logging.getLogger(__name__)

# JWKS client — caches keys automatically, only fetches when needed.
_jwks_client: Optional[PyJWKClient] = None


def _get_jwks_client() -> Optional[PyJWKClient]:
    """Lazily initialise the JWKS client from the Supabase URL."""
    global _jwks_client
    if _jwks_client is not None:
        return _jwks_client
    if not settings.supabase_url:
        return None
    jwks_url = f"{settings.supabase_url}/auth/v1/.well-known/jwks.json"
    _jwks_client = PyJWKClient(jwks_url, cache_keys=True)
    return _jwks_client


def _extract_token(request: Request) -> Optional[str]:
    """Pull Bearer token from the Authorization header."""
    auth = request.headers.get("authorization", "")
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return None


def _decode_token(token: str) -> dict:
    """Decode and verify a Supabase JWT.

    Strategy:
      1. Try JWKS (ES256/RS256) — works for newer Supabase projects.
      2. Fall back to HS256 with the shared JWT secret.
    """
    # --- Try JWKS first (asymmetric: ES256 / RS256) ---
    jwks = _get_jwks_client()
    if jwks is not None:
        try:
            signing_key = jwks.get_signing_key_from_jwt(token)
            return jwt.decode(
                token,
                signing_key.key,
                algorithms=["ES256", "RS256", "EdDSA"],
                audience="authenticated",
            )
        except Exception as exc:
            log.warning("JWKS verification failed (%s: %s), trying HS256 fallback",
                        type(exc).__name__, exc)
    else:
        log.warning("JWKS client not available (supabase_url=%r)", settings.supabase_url)

    # --- Fallback: HS256 with shared secret ---
    if settings.supabase_jwt_secret:
        return jwt.decode(
            token,
            settings.supabase_jwt_secret,
            algorithms=["HS256"],
            audience="authenticated",
        )

    raise jwt.InvalidTokenError("No verification method available (JWKS failed, no HS256 secret)")


async def get_current_user(request: Request) -> str:
    """FastAPI dependency — requires a valid JWT. Returns user_id (UUID str)."""
    token = _extract_token(request)
    if not token:
        raise HTTPException(status_code=401, detail="Missing authorization header")
    try:
        payload = _decode_token(token)
        user_id = payload.get("sub")
        if not user_id:
            raise HTTPException(status_code=401, detail="Invalid token: no sub claim")
        return user_id
    except jwt.ExpiredSignatureError:
        raise HTTPException(status_code=401, detail="Token expired")
    except jwt.InvalidTokenError as e:
        raise HTTPException(status_code=401, detail=f"Invalid token: {e}")


async def get_optional_user(request: Request) -> Optional[str]:
    """FastAPI dependency — returns user_id or None for unauthenticated callers."""
    token = _extract_token(request)
    if not token:
        return None
    try:
        payload = _decode_token(token)
        return payload.get("sub")
    except Exception:
        return None
