#!/bin/bash
# Issue #64 held-out validation on SKA SDC2 ldev (INAF PLEIADI-GPU, POWER9/ppc64le).
#   sbatch validation/sdc2/run_sdc2_pleiadi.sbatch      (from the synced checkout on the frontend)
# Stop it with: scancel <jobid>   (squeue -u $USER shows it as visivo-64-sdc2)
#
# Validates the REVISED rules (spec §7, rev. 2026-11-05b: R2 kernelsXY, C2 wide-band contsub,
# no dilation) with the frozen criteria P1-P4; the driver refuses to run unless the app's
# suggestion implements §7.
# Prerequisites (one-off, on the frontend, see validation/out/sofia_suggest/RESULTS.md "SDC2"):
#   1. $W/sofia2/sofia — SoFiA-2 2.7.0 built from source INSIDE $SIF (the Docker image is
#      amd64-only; the binary needs the container's glibc 2.39): WCSLIB 8.10 static in $W/wcslib,
#      `make OMP=true CPPFLAGS=-I$W/wcslib/include LDFLAGS="-lm $W/wcslib/lib/libwcs.a"`.
#   2. $W/truth/sky_ldev_truthcat_v2.txt — the SDC2 ldev truth catalogue (SDC2 data release).
#   3. ska-sdc 2.0.0 in $W/pylib (pip install --no-deps --target) and a conda-forge env
#      $W/scoreenv (python 3.12, numpy 1.26, pandas<2.3, scipy, scikit-learn, astropy,
#      setuptools<81) for the score stage: the backend image has no scikit-learn, PyPI has no
#      ppc64le wheel for it, and ska-sdc imports pkg_resources.
#   4. the worktree (branch 64-...) synced to $W/VisIVO-next-64 (backend + validation/).
#SBATCH -J visivo-64-sdc2
#SBATCH -p gpu
#SBATCH --gres=gpu:1
#SBATCH -c 32
#SBATCH --mem=64G
#SBATCH -t 12:00:00
#SBATCH -o /scratch/fvitello/skava-e2e/issue64/sdc2-%j.log
set -euo pipefail
W=/scratch/fvitello/skava-e2e/issue64
CUBE=/mnt/beegfs/fvitello/demo_data/sky_ldev_v2.fits      # read-only, never copied
SIF=/scratch/fvitello/skava-e2e/backend-conda-gpu.sif
OUT=$W/out
mkdir -p "$OUT" "$W/exports"
hostname >&2
ls -ldn /scratch >&2   # the shared /scratch mount has vanished on nodes before (2026-11-02)

TOK=$(python3 -c "import secrets;print(secrets.token_urlsafe(24))")
export APPTAINERENV_VISIVO_TOKEN="$TOK"
export APPTAINERENV_VISIVO_DATA_ROOTS=/mnt/beegfs/fvitello/demo_data:$W
export APPTAINERENV_VISIVO_SOFIA_BIN=$W/sofia2/sofia
export APPTAINERENV_VISIVO_EXPORTS_DIR=$W/exports
export APPTAINERENV_VISIVO_FINDER_TIMEOUT_S=36000
B="-B /mnt/beegfs -B /scratch"

apptainer exec $B --pwd $W/VisIVO-next-64/backend "$SIF" \
    python -m uvicorn app.main:app --host 127.0.0.1 --port 18764 > "$OUT/backend.log" 2>&1 &
BACK=$!
trap 'kill $BACK 2>/dev/null' EXIT
READY=0
for _ in $(seq 120); do
    kill -0 "$BACK" 2>/dev/null || break
    if curl -sf --connect-timeout 5 --max-time 20 -o /dev/null -H "X-Visivo-Token: $TOK" \
            http://127.0.0.1:18764/v1/finders; then READY=1; break; fi
    sleep 2
done
if [ "$READY" != 1 ]; then echo "backend did not start; see $OUT/backend.log" >&2; exit 1; fi

export APPTAINERENV_VISIVO_URL=http://127.0.0.1:18764
T0=$(date +%s)
apptainer exec $B "$SIF" env VISIVO_TOKEN="$TOK" \
    python $W/VisIVO-next-64/validation/sdc2/sofia_suggest_sdc2.py run --cube "$CUBE" --out "$OUT"
echo "run stage: $(( $(date +%s) - T0 )) s" >&2
apptainer exec $B "$SIF" env PYTHONPATH=$W/pylib \
    $W/scoreenv/bin/python $W/VisIVO-next-64/validation/sdc2/sofia_suggest_sdc2.py score --cube "$CUBE" \
        --truth $W/truth/sky_ldev_truthcat_v2.txt --out "$OUT"
echo "total: $(( $(date +%s) - T0 )) s" >&2
