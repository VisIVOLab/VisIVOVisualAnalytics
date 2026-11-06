#!/bin/bash
#SBATCH -p gpu --gres=gpu:1 -c 8 -t 03:00:00 -J b3gen450
#SBATCH -o /mnt/beegfs/fvitello/visivo/bench3/gen450_%j.log
cd /tmp
grep MemTotal /proc/meminfo
/usr/bin/time -v apptainer exec -B /mnt/beegfs /mnt/beegfs/fvitello/visivo/backend-egl.sif python \
  /mnt/beegfs/fvitello/visivo/checkout-3/benchmark/make_synthetic_cube.py --gb 450 --spectral freq \
  --out /mnt/beegfs/fvitello/visivo/bench3/syn/cube_450gb_freq.fits
ls -l /mnt/beegfs/fvitello/visivo/bench3/syn/
