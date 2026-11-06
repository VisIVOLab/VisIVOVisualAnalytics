"""Where the VLKB lives — one rule for every route that talks to it.

The TAP query composer and the multi-cutout batch reach the same archive, so
they must agree on where it is. They used not to: the query fell back to a
built-in address and the batch had no fallback at all, so a backend with
``VISIVO_VLKB_URL`` unset could search the archive and then refuse to cut from
what it had found.

The rule, for both: the URL the request carries, else ``VISIVO_VLKB_URL``, else
:data:`DEFAULT_VLKB_URL`. The default is the service root the desktop client
also uses (its ``VLKB/url`` setting), under which ``/tap``, ``/soda`` and
``/siav2`` live. The SSRF guard is not applied here: each caller checks the URL
it is about to open, with ``VISIVO_VLKB_ALLOWED_HOSTS``.
"""
from __future__ import annotations

import os
from urllib.parse import urljoin

#: The public VLKB service root at INAF-IA2. Its TAP service answers at
#: ``<root>/tap/sync`` and its batch cutouts at ``<root>/soda/async/uws``.
DEFAULT_VLKB_URL = "https://vlkb.ia2.inaf.it"


def vlkb_service_url(explicit: str = "") -> str:
    """The VLKB base URL for a request: its own, the configured one, or the default.

    Returned without a trailing slash and with ``.``/``..`` segments resolved,
    so ``<base>/tap/sync`` and ``<base>/soda/async/uws`` are well formed
    whichever way the URL was written.
    """
    url = ((explicit or "").strip()
           or os.environ.get("VISIVO_VLKB_URL", "").strip()
           or DEFAULT_VLKB_URL)
    return urljoin(url + "/", ".").rstrip("/")
