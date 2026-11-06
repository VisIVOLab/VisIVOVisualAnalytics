#!/bin/bash
#SBATCH -p gpu --gres=gpu:1 -c 32 --exclusive -t 04:00:00 -J b3s124L
#SBATCH -o /mnt/beegfs/fvitello/visivo/bench3/sky124_local_%j.log
L=/tmp/fvitello_b3_$SLURM_JOB_ID
mkdir -p $L && trap "rm -rf $L" EXIT; trap "exit 143" TERM INT
cd /tmp; df -hT /tmp
t0=$(date +%s); cp /mnt/beegfs/astrodata/FITS/Sky124/sky124.fits $L/; echo "copy $(( $(date +%s)-t0 )) s"
C=/mnt/beegfs/fvitello/visivo/checkout-3
SIF=/mnt/beegfs/fvitello/visivo/backend-egl.sif BINDS=/mnt/beegfs,/tmp CAP_GB=16 MASK=7.3e-3 PY=python REPEATS=2 \
bash $C/benchmark/remote/run_hpc_moment_bench.sh /mnt/beegfs/fvitello/visivo/bench3/sky124_local_$SLURM_JOB_ID \
  sky124_local=$L/sky124.fits
