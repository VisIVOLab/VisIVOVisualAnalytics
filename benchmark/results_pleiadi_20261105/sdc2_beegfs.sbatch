#!/bin/bash
#SBATCH -p gpu --gres=gpu:1 -c 32 --exclusive -t 02:30:00 -J b3sdc2B
#SBATCH -o /mnt/beegfs/fvitello/visivo/bench3/sdc2_beegfs_%j.log
cd /tmp
C=/mnt/beegfs/fvitello/visivo/checkout-3
SIF=/mnt/beegfs/fvitello/visivo/backend-egl.sif BINDS=/mnt/beegfs CAP_GB=16 MASK=8.6e-5 PY=python \
REPEATS=3 CHECK_CHANNELS=3630:3757 \
bash $C/benchmark/remote/run_hpc_moment_bench.sh /mnt/beegfs/fvitello/visivo/bench3/sdc2_beegfs_$SLURM_JOB_ID \
  sdc2_beegfs=/mnt/beegfs/fvitello/demo_data/sky_ldev_v2.fits
