#!/bin/bash
#SBATCH -p gpu --gres=gpu:1 -c 32 --exclusive -t 04:00:00 -J b3s124B
#SBATCH -o /mnt/beegfs/fvitello/visivo/bench3/sky124_beegfs_%j.log
cd /tmp
C=/mnt/beegfs/fvitello/visivo/checkout-3
SIF=/mnt/beegfs/fvitello/visivo/backend-egl.sif BINDS=/mnt/beegfs CAP_GB=16 MASK=7.3e-3 PY=python REPEATS=2 \
bash $C/benchmark/remote/run_hpc_moment_bench.sh /mnt/beegfs/fvitello/visivo/bench3/sky124_beegfs_$SLURM_JOB_ID \
  sky124_beegfs=/mnt/beegfs/astrodata/FITS/Sky124/sky124.fits
