"""
auth.py – R1: Bearer-token authentication for the VisIVO backend.

A token is loaded from the VISIVO_TOKEN environment variable.
If the variable is absent a cryptographically-random token is generated
at startup, printed to stdout AND written to ~/.visivo_token so the
Qt client can read it automatically.

Every API route that uses `Depends(verify_token)` will return 401
if the caller omits or supplies the wrong `X-Visivo-Token` header.
"""

from __future__ import annotations

import errno
import logging
import os
import secrets
from pathlib import Path

from fastapi import Depends, HTTPException, Security, status
from fastapi.security import APIKeyHeader

logger = logging.getLogger("visivo.auth")

_TOKEN_FILE = Path.home() / ".visivo_token"
_HEADER_NAME = "X-Visivo-Token"

_api_key_header = APIKeyHeader(name=_HEADER_NAME, auto_error=False)


def _resolve_token() -> str:
    """Return the active bearer token, generating one if necessary.

    Always prints the active token to stdout at startup so the operator can
    copy-paste it into the client even when `VISIVO_TOKEN` was inherited
    from the environment (otherwise the only feedback would be a logger.info
    that is suppressed when logging.basicConfig has not been called yet —
    which is the case during module-import in main.py).
    """
    env_token = os.environ.get("VISIVO_TOKEN", "").strip()
    if env_token:
        print(  # noqa: T201 – intentional: visible at startup
            f"\n[VisIVO] Backend token (from VISIVO_TOKEN env): {env_token}\n"
        )
        return env_token

    generated = secrets.token_urlsafe(32)
    written = False
    try:
        _TOKEN_FILE.write_text(generated + "\n")
        _TOKEN_FILE.chmod(0o600)
        written = True
    except OSError as exc:
        # EROFS (read-only fs) and EACCES (permission denied) are the
        # expected outcome inside an Apptainer image: there's no
        # writable HOME and operators pass the token via VISIVO_TOKEN
        # anyway. Keep silent in those cases.
        if exc.errno not in (errno.EROFS, errno.EACCES):
            print(f"\n[VisIVO] WARNING: could not write {_TOKEN_FILE}: {exc}\n")  # noqa: T201

    msg = (
        f"\n[VisIVO] Backend token: {generated}\n"
        f"[VisIVO] Or set: export VISIVO_TOKEN={generated}\n"
    )
    if written:
        msg += f"[VisIVO] Token written to {_TOKEN_FILE} (mode 600).\n"
    print(msg)  # noqa: T201 – intentional: operators need to see the token at startup
    return generated


# Module-level singleton – resolved once at import time.
TOKEN: str = _resolve_token()


def _resolve_tokens() -> tuple[str, ...]:
    """The set of ACCEPTED tokens: the primary TOKEN plus any additional per-user
    tokens in ``VISIVO_TOKENS`` (comma/whitespace-separated).

    Re-read from the environment on every call so an operator can revoke (or add)
    a per-user token without restarting the process. With only the single primary
    token this is a one-element tuple — unchanged behaviour — but distinct
    per-user tokens let a shared backend attribute each request to a user and
    isolate their sessions (see sessions.py owner binding).
    """
    # The primary token is re-read too, so rotating VISIVO_TOKEN takes effect
    # without a restart; when it is unset we keep the id generated at startup.
    primary = os.environ.get("VISIVO_TOKEN", "").strip() or TOKEN
    extra = os.environ.get("VISIVO_TOKENS", "")
    tokens = [primary]
    for t in extra.replace(",", " ").split():
        t = t.strip()
        if t and t not in tokens:
            tokens.append(t)
    return tuple(tokens)


def accepted_tokens() -> tuple[str, ...]:
    """Currently accepted tokens (live). Tests may monkeypatch this."""
    return _resolve_tokens()


def multi_user_mode() -> bool:
    """True when more than one token is accepted — the backend is shared by
    distinct users, so per-user isolation must be enforced strictly."""
    return len(accepted_tokens()) > 1


def primary_token() -> str:
    """The operator/admin token: the primary VISIVO_TOKEN (live) or, when unset,
    the id generated at startup."""
    return os.environ.get("VISIVO_TOKEN", "").strip() or TOKEN


def is_admin_token(token: str | None) -> bool:
    """True for the operator. With a single token everyone is the operator (the
    single-user backend has nothing to separate); once per-user tokens exist only
    the PRIMARY one may read server-wide ops data (all sessions, all log lines)."""
    if not multi_user_mode():
        return True
    return token is not None and secrets.compare_digest(token, primary_token())


# Import-time snapshot kept for introspection/back-compat; prefer accepted_tokens().
TOKENS: tuple[str, ...] = _resolve_tokens()


async def verify_token(
    x_visivo_token: str | None = Security(_api_key_header),
) -> str:
    """FastAPI dependency: raise 401 if the request token is wrong or absent;
    otherwise return the MATCHED token (the caller's identity, used to bind and
    guard session ownership).

    Every accepted token is compared with ``secrets.compare_digest`` and the loop
    does NOT short-circuit on a match, so neither the matching position nor the
    number of configured tokens is observable through response timing.
    """
    matched: str | None = None
    if x_visivo_token is not None:
        for accepted in accepted_tokens():
            if secrets.compare_digest(x_visivo_token, accepted):
                matched = accepted   # keep looping: same work whether or not it hit
    if matched is not None:
        return matched
    raise HTTPException(
        status_code=status.HTTP_401_UNAUTHORIZED,
        detail="Missing or invalid X-Visivo-Token header.",
        headers={"WWW-Authenticate": f'ApiKey realm="{_HEADER_NAME}"'},
    )


async def require_operator(token: str = Depends(verify_token)) -> str:
    """Dependency for SERVER-WIDE resources (all sessions, the shared log buffer,
    the machine-wide SAMP hub, the shared Workspace Exports folder).

    On a single-token backend everyone is the operator, so behaviour is unchanged;
    once per-user tokens are configured only the PRIMARY token may reach them.
    """
    if not is_admin_token(token):
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="Operator token required for this server-wide resource.",
        )
    return token
