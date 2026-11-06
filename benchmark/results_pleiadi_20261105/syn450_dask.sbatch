#!/bin/bash
#SBATCH -p gpu --gres=gpu:1 -c 32 --exclusive -t 04:00:00 -J b3s450D
#SBATCH -o /mnt/beegfs/fvitello/visivo/bench3/syn450_dask_%j.log
cd /tmp
C=/mnt/beegfs/fvitello/visivo/checkout-3
SIF=/mnt/beegfs/fvitello/visivo/backend-egl.sif BINDS=/mnt/beegfs CAP_GB=16 MASK=3.0 PY=python REPEATS=2 WAYS=dask \
bash $C/benchmark/remote/run_hpc_moment_bench.sh /mnt/beegfs/fvitello/visivo/bench3/syn450_dask_$SLURM_JOB_ID \
  syn450_beegfs=/mnt/beegfs/fvitello/visivo/bench3/syn/cube_450gb_freq.fits
