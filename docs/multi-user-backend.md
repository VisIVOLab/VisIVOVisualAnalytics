# Sharing one backend between users

A VisIVO backend started with a single token is a personal server: it acts on
behalf of the one person running it. Configure more than one token and it becomes
a shared server — and the isolation rules below switch on automatically, because
"more than one token" is exactly the signal that the tokens belong to different
people.

This page is for whoever operates the backend. It says what is isolated, what is
deliberately shared, and what to configure before pointing untrusted users at it.

## Turning it on

```bash
export VISIVO_TOKEN=<operator-token>              # the primary / operator identity
export VISIVO_TOKENS=<alice-token>,<bob-token>    # additional per-user tokens
export VISIVO_DATA_ROOTS=/srv/survey:/srv/shared  # what the filesystem exposes
```

`VISIVO_TOKENS` accepts a comma- or whitespace-separated list; `VISIVO_TOKEN` is
always accepted too and is the **operator** token. Multi-user mode is on whenever
the total number of accepted tokens is greater than one — there is no separate
switch to forget.

Generate tokens with at least 32 bytes of entropy:

```bash
python -c "import secrets; print(secrets.token_urlsafe(32))"
```

Every token is compared with `secrets.compare_digest` against the full accepted
list without short-circuiting, so a wrong token costs the same time whichever
position it would have matched.

## What each user gets

**Their own sessions.** Every desktop client mints a session at
`POST /v1/sessions/new` and sends it as `X-Visivo-Session`. The session is bound
to the token that minted it, and a request carrying someone else's session id
gets **404, not 403** — so ids are not enumerable and one user cannot even
confirm that another's session exists. Datasets, render sessions, tasks,
diagnostics snapshots and copilot conversations all hang off that session.

**Their own copilot configuration.** Provider choice, model and API key are held
per session, in backend memory only: never logged, never returned by
`/v1/assistant/status`, never written to disk. Alice configuring Claude does not
change what Bob's copilot talks to, and neither can read the other's key.

**A session quota.** Each token is capped at a fixed number of live sessions;
exceeding it returns 429 rather than letting one client exhaust the server.

In multi-user mode the client MUST send `X-Visivo-Session`: the shared anonymous
session is refused (400), and a client-invented id is not adopted — only ids the
backend minted are accepted (`VISIVO_STRICT_SESSIONS` defaults to on here, and
can be forced either way).

## What stays shared: the operator's resources

Some things are properties of the machine, not of a user, and are gated behind
the **operator** token — `VISIVO_TOKEN`. A non-operator token gets 403:

- the full session list and server-wide `recent_tasks` in `/v1/health`
- the diagnostics log buffer (it interleaves every user's activity)
- the SAMP hub (one per machine, and it talks to local desktop apps)
- the Workspace Exports folder: listing, downloading and deleting

With a single token this changes nothing, because there everyone is the operator.

Log lines are scrubbed at the buffer's choke point: session ids, tokens and other
high-entropy strings are truncated to their first 8 characters before they are
stored, so the operator log cannot leak a credential either.

## What is NOT isolated: the filesystem

This is the part worth reading twice. A token authenticates a user; it does not
give them a private disk. Path-based endpoints — the file browser, FITS header
peeks, catalogue and HiPS overlay reads — all resolve a client-supplied path on
the server, and **any accepted token can read anything inside the allowed
roots**. There is no per-user ownership of files.

`VISIVO_DATA_ROOTS` is therefore the boundary, and its default depends on the
mode:

| | `VISIVO_DATA_ROOTS` set | unset |
|---|---|---|
| single token | confined to those roots | **unconfined** — the backend is the user's own machine |
| multiple tokens | confined to those roots | **all path access denied** |

Multi-user mode fails closed: with no roots declared, the file browser, dataset
open and the path-based overlays refuse everything, and the backend says so in a
warning at startup. There is no implicit default, deliberately — falling back to
the service account's home would silently share whatever happens to live in it.
**Declaring `VISIVO_DATA_ROOTS` is part of setting up a shared backend.** Point it
at the survey directories the group is meant to share, and nothing else.

In multi-user mode, hidden entries (any path component starting with `.`) are
also refused and filtered out of directory listings, so `~/.visivo_token` and
`~/.ssh` are neither readable nor visible even when a root contains them.

Refusals never announce themselves as refusals: a route that builds a
`valid=False` envelope reports the same "not found" it would for an absent file,
and anything else answers **404, not 403**. The status code alone would otherwise
confirm that an out-of-bounds path exists.

What confinement does NOT give you is per-user ownership: everyone with an
accepted token sees the same roots. If your users need genuinely private data,
run one backend per user (each with its own token and its own
`VISIVO_DATA_ROOTS`) rather than one backend with several tokens. Token isolation
covers per-session state; it is not a per-user filesystem sandbox, and this page
would be lying if it implied otherwise.

## Checking a deployment

```bash
# operator sees the whole picture
curl -s -H "X-Visivo-Token: $OPERATOR" localhost:8000/v1/health | jq .recent_tasks

# a user token does not
curl -s -o /dev/null -w '%{http_code}\n' \
     -H "X-Visivo-Token: $ALICE" localhost:8000/v1/exports/list      # 403

# a user cannot reach outside the roots
curl -s -H "X-Visivo-Token: $ALICE" \
     "localhost:8000/v1/files/list?path=/etc" | jq .valid            # false

# nor borrow someone else's session
curl -s -o /dev/null -w '%{http_code}\n' \
     -H "X-Visivo-Token: $ALICE" \
     localhost:8000/v1/sessions/$BOB_SESSION/datasets                 # 404
```
