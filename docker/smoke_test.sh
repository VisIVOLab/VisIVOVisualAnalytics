#!/usr/bin/env bash
# Smoke test of a backend image, as CI runs it and as anyone can run it:
#
#   docker/smoke_test.sh IMAGE
#
# Starts the image the way a locked-down site would (not root, read-only root
# file system, the data bound read-only), waits for /v1/health, opens a small
# FITS cube through the API, and checks that a file outside the data directory
# is refused. It also checks that the image refuses to start with no data
# directory, and that a modified copy of backend/app bound onto /app/app (what
# the launcher's --backend-src does) is the code that runs. Needs Docker (or
# DOCKER=podman), curl and python3 on the host.
set -euo pipefail

IMAGE="${1:?usage: smoke_test.sh IMAGE}"
DOCKER="${DOCKER:-docker}"
PORT="${SMOKE_PORT:-18765}"
TOKEN="smoke-$RANDOM-$RANDOM"
WORK="$(mktemp -d)"
NAME="visivo-smoke-$$"
NAME2="$NAME-overlay"

cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then
        for c in "$NAME" "$NAME2"; do
            echo "--- log of $c ---"
            "$DOCKER" logs "$c" 2>&1 | sed "s/${TOKEN}/<token>/g" | tail -n 40 || true
        done
    fi
    "$DOCKER" rm -f "$NAME" "$NAME2" >/dev/null 2>&1 || true
    rm -rf "$WORK"
    exit "$status"
}
trap cleanup EXIT

# The test cube is written by the image's own astropy, as the host user, so
# that the bind mount is writable for it and the file is the user's.
mkdir -p "$WORK/data"
"$DOCKER" run --rm -u "$(id -u):$(id -g)" -v "$WORK/data:/data" "$IMAGE" python -c "
import numpy as np
from astropy.io import fits
cube = np.random.default_rng(1).normal(size=(8, 32, 48)).astype('float32')
h = fits.Header()
for k, v in {'CTYPE1': 'RA---SIN', 'CTYPE2': 'DEC--SIN', 'CTYPE3': 'VRAD',
             'CRVAL1': 150.0, 'CRVAL2': 2.0, 'CRVAL3': 1.0e4,
             'CRPIX1': 24, 'CRPIX2': 16, 'CRPIX3': 1,
             'CDELT1': -0.001, 'CDELT2': 0.001, 'CDELT3': 5.0e3,
             'CUNIT1': 'deg', 'CUNIT2': 'deg', 'CUNIT3': 'm/s', 'BUNIT': 'Jy/beam'}.items():
    h[k] = v
fits.PrimaryHDU(cube, h).writeto('/data/smoke.fits')
"

# With no data directory (nothing at /data, no VISIVO_DATA_ROOT[S]) the backend
# would open any file its user can read: the entrypoint refuses to start, and
# VISIVO_ALLOW_UNRESTRICTED=1 is the operator's explicit way round it.
set +e
out="$("$DOCKER" run --rm -u "$(id -u):$(id -g)" -e VISIVO_TOKEN="$TOKEN" "$IMAGE" 2>&1)"
status=$?
set -e
if [ "$status" -eq 0 ] || ! echo "$out" | grep -q "VISIVO_ALLOW_UNRESTRICTED"; then
    echo "an image with no data directory did not refuse to start (exit $status)"; exit 1
fi
echo "no data directory: refused to start"

# --read-only with a tmpfs /tmp is the strictest case: Apptainer mounts the image
# read-only too. The entrypoint's scratch directory is in /tmp.
"$DOCKER" run -d --name "$NAME" --read-only --tmpfs /tmp \
    -u "$(id -u):$(id -g)" \
    -v "$WORK/data:/data:ro" \
    -e VISIVO_TOKEN="$TOKEN" \
    -p "127.0.0.1:${PORT}:8000" \
    "$IMAGE" >/dev/null

url="http://127.0.0.1:${PORT}"
for i in $(seq 1 120); do
    if curl -sf -H "X-Visivo-Token: $TOKEN" "$url/v1/health" >/dev/null 2>&1; then
        break
    fi
    if [ "$("$DOCKER" inspect -f '{{.State.Running}}' "$NAME")" != "true" ]; then
        echo "the container exited before it answered"; exit 1
    fi
    [ "$i" -lt 120 ] || { echo "no answer from /v1/health in 120 s"; exit 1; }
    sleep 1
done
echo "health: ok"

# The token is required.
code="$(curl -s -o /dev/null -w '%{http_code}' "$url/v1/health")"
case "$code" in 401|403) echo "no token: refused ($code)";; *) echo "no token got $code"; exit 1;; esac

open() {
    curl -sf -X POST -H "X-Visivo-Token: $TOKEN" -H 'Content-Type: application/json' \
        -d "{\"path\": \"$1\"}" "$url/v1/datasets/open"
}

reply="$(open /data/smoke.fits)"
echo "$reply" | python3 -c "
import json, sys
r = json.load(sys.stdin)
assert r['valid'], r['error']
assert (r['width'], r['height'], r['depth']) == (48, 32, 8), r
print('open: a', r['kind'] or 'dataset', r['width'], 'x', r['height'], 'x', r['depth'], 'cube')
"

# Confined to the bound data directory (docker/entrypoint.sh): the backend
# answers a path outside it as it answers one that does not exist.
code="$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Visivo-Token: $TOKEN" \
    -H 'Content-Type: application/json' -d '{"path": "/etc/passwd"}' "$url/v1/datasets/open")"
case "$code" in
    403|404) echo "outside the data directory: refused ($code)";;
    *) echo "a path outside the data directory got $code"; exit 1;;
esac

# The launcher scripts' --backend-src binds a host copy of backend/app onto
# /app/app. Take the image's own code as that copy and change its version, which
# the API reports (the OpenAPI document's info.version): the backend runs the
# bound copy if it says so, while the one above, the image's own, does not.
version_of() {
    curl -sf -H "X-Visivo-Token: $TOKEN" "$1/v1/openapi.json" 2>/dev/null \
        | python3 -c 'import json, sys; print(json.load(sys.stdin)["info"]["version"])' 2>/dev/null || true
}
shipped="$(version_of "$url")"
mkdir -p "$WORK/app"
"$DOCKER" run --rm -u "$(id -u):$(id -g)" -v "$WORK/app:/out" "$IMAGE" cp -r /app/backend/app/. /out/
echo "9.9.9" > "$WORK/app/VERSION"
"$DOCKER" run -d --name "$NAME2" --read-only --tmpfs /tmp \
    -u "$(id -u):$(id -g)" \
    -v "$WORK/data:/data:ro" \
    -v "$WORK/app:/app/app:ro" \
    -e VISIVO_TOKEN="$TOKEN" \
    -p "127.0.0.1:$((PORT + 1)):8000" \
    "$IMAGE" >/dev/null
url2="http://127.0.0.1:$((PORT + 1))"
bound=""
for i in $(seq 1 120); do
    bound="$(version_of "$url2")"
    [ -z "$bound" ] || break
    if [ "$("$DOCKER" inspect -f '{{.State.Running}}' "$NAME2")" != "true" ]; then
        echo "the container with /app/app bound exited before it answered"; exit 1
    fi
    [ "$i" -lt 120 ] || { echo "no answer in 120 s from the container with /app/app bound"; exit 1; }
    sleep 1
done
if [ "$bound" != "9.9.9" ] || [ "$shipped" = "9.9.9" ]; then
    echo "/app/app bound, but the backend reports $bound (the image's own: $shipped)"; exit 1
fi
echo "/app/app bound: the backend runs the bound copy (version $bound, the image's $shipped)"
echo "smoke test passed: $IMAGE"
