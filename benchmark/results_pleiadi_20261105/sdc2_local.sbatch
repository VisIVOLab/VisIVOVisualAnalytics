#!/bin/bash
#SBATCH -p gpu --gres=gpu:1 -c 32 --exclusive -t 02:30:00 -J b3sdc2L
#SBATCH -o /mnt/beegfs/fvitello/visivo/bench3/sdc2_local_%j.log
L=/tmp/fvitello_b3_$SLURM_JOB_ID
mkdir -p $L && trap "rm -rf $L" EXIT
cd /tmp; df -hT /tmp
t0=$(date +%s); cp /mnt/beegfs/fvitello/demo_data/sky_ldev_v2.fits $L/; echo "copy $(( $(date +%s)-t0 )) s"
C=/mnt/beegfs/fvitello/visivo/checkout-3
SIF=/mnt/beegfs/fvitello/visivo/backend-egl.sif BINDS=/mnt/beegfs,/tmp CAP_GB=16 MASK=8.6e-5 PY=python \
REPEATS=3 \
bash $C/benchmark/remote/run_hpc_moment_bench.sh /mnt/beegfs/fvitello/visivo/bench3/sdc2_local_$SLURM_JOB_ID \
  sdc2_local=$L/sky_ldev_v2.fits
