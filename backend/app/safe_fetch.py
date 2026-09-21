"""One way to fetch a URL the client chose — guard included, redirects too.

``assert_safe_fetch_url`` checks the URL the client sent. That is not enough on
its own: ``urlopen`` follows redirects by default, and a perfectly public host
is free to answer ``302 Location: http://169.254.169.254/`` or a loopback
address. The check has to run again on every hop, which means an opener rather
than a bare ``urlopen`` — and that is the kind of detail that is remembered in
one subsystem and forgotten in the next (it has already been fixed twice here,
in the SED model fetch and in the VLKB cutout client).

So it lives here, once, and the archive routes call it.
"""
from __future__ import annotations

import urllib.request
from typing import Any

from .dependencies import assert_safe_fetch_url


def _redirect_handler(allowlist_env: str) -> type[urllib.request.HTTPRedirectHandler]:
    class _ValidatingRedirectHandler(urllib.request.HTTPRedirectHandler):
        """Re-run the SSRF check on every redirect target."""

        def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102, ANN001
            try:
                assert_safe_fetch_url(newurl, allowlist_env=allowlist_env, block_private=True)
            except ValueError:
                # urllib closes `fp` on the paths it controls; raising from here
                # is not one of them, and leaving the socket to the garbage
                # collector keeps it alive for as long as the traceback does.
                try:
                    fp.close()
                except Exception:  # noqa: BLE001 — closing is best-effort
                    pass
                raise
            return super().redirect_request(req, fp, code, msg, headers, newurl)

    return _ValidatingRedirectHandler


def safe_urlopen(url: str, *, allowlist_env: str, timeout: float = 120.0,
                 headers: dict[str, str] | None = None) -> Any:
    """Open ``url`` with the SSRF policy applied to it *and* to every redirect.

    Raises ``ValueError`` (from the guard) before any connection is made when
    the URL itself is refused; ``urllib`` errors otherwise, for the caller to
    turn into its own message.
    """
    assert_safe_fetch_url(url, allowlist_env=allowlist_env)
    opener = urllib.request.build_opener(_redirect_handler(allowlist_env))
    request = urllib.request.Request(url, headers=headers or {})  # noqa: S310 — checked above
    return opener.open(request, timeout=timeout)
