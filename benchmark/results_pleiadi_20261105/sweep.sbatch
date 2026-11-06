#!/bin/bash
#SBATCH -p gpu --gres=gpu:1 -c 32 --exclusive -t 01:30:00 -J b3sweep
#SBATCH -o /mnt/beegfs/fvitello/visivo/bench3/sweep_%j.log
L=/tmp/fvitello_b3_$SLURM_JOB_ID
mkdir -p $L && trap "rm -rf $L" EXIT; trap "exit 143" TERM INT
cd /tmp; t0=$(date +%s); cp /mnt/beegfs/fvitello/demo_data/sky_ldev_v2.fits $L/; echo "copy $(( $(date +%s)-t0 )) s"
C=/mnt/beegfs/fvitello/visivo/checkout-3
for t in 1 4 8 16 32 64; do
SIF=/mnt/beegfs/fvitello/visivo/backend-egl.sif BINDS=/mnt/beegfs,/tmp CAP_GB=16 MASK=8.6e-5 PY=python \
REPEATS=0 WAYS=dask ORDERS="0 2" THREADS=$t CAP_PROOF=0 \
bash $C/benchmark/remote/run_hpc_moment_bench.sh /mnt/beegfs/fvitello/visivo/bench3/sweep_$SLURM_JOB_ID/t$t sdc2_local=$L/sky_ldev_v2.fits
done
