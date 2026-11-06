#!/bin/bash
#SBATCH -p gpu --gres=gpu:1 -c 32 --exclusive -t 02:00:00 -J b3s450F
#SBATCH -o /mnt/beegfs/fvitello/visivo/bench3/syn450_fixed_%j.log
cd /tmp
C=/mnt/beegfs/fvitello/visivo/checkout-3b
S=/mnt/beegfs/fvitello/visivo/backend-egl.sif
O=/mnt/beegfs/fvitello/visivo/bench3/syn450_fixed_$SLURM_JOB_ID
mkdir -p $O
apptainer exec --cleanenv --env VISIVO_TOKEN=bench,OPENBLAS_NUM_THREADS=4 -B /mnt/beegfs $S python $C/benchmark/moment_ooc_bench.py check \
  --cube /mnt/beegfs/fvitello/demo_data/sky_ldev_v2.fits --channels 3630:3757 --mask 8.6e-5 \
  --backend $C/backend --criterion $C/benchmark/moment_criterion_ulp.py --out $O/check_sdc2.json
SIF=$S BINDS=/mnt/beegfs CAP_GB=16 MASK=3.0 PY=python REPEATS=0 WAYS=dask CAP_PROOF=0 \
bash $C/benchmark/remote/run_hpc_moment_bench.sh $O syn450_beegfs=/mnt/beegfs/fvitello/visivo/bench3/syn/cube_450gb_freq.fits
