# Remote and memory-capped benchmark

The single-host campaign (`../run_local_bench.sh`) measures the embedded
service on the client's own machine. This one runs the same
`benchmark_visivo.py` against a service that is not the client's:

- **a Linux container with a hard memory cap** (default 8 GB, swap disabled),
  so a 30 GB cube is larger than the memory the service may use. The cgroup's
  `memory.current` is sampled during every operation and `memory.peak`,
  `memory.max` and the OOM-kill count are recorded at the end;
- **a shaped link**: `tc netem` on the container's egress adds a fixed delay
  (default 20 ms per round trip) and caps the bandwidth (default 1 Gbit/s),
  the order of a national research-network path between two sites.

It is an emulation, not two machines: client and container share the host's
CPU and SSD, and the cube is read through Docker's file sharing. What it does
establish is the behaviour of the service under a memory cap smaller than the
cube, and the latency and bytes a client pays over a link of known capacity.
The same script works unchanged against a real remote host by pointing
`--base-url` of `benchmark_visivo.py` at it.

## Run

```bash
docker build -f benchmark/remote/Dockerfile -t visivo-bench-backend .
benchmark/remote/run_remote_bench.sh ~/visivo_bench_data benchmark/results_remote_$(date +%Y%m%d) \
    cube_1gb.fits cube_8gb.fits cube_30gb.fits
```

Knobs: `MEM`, `CPUS`, `WORKERS`, `DELAY`, `RATE`, `REPEATS` (see the script).
Each cube gets a fresh container. Per cube the output directory holds
`results.json` and `results.md` from the benchmark (timings, bytes sent back
per operation, peak memory per operation, slice latency idle and while a masked
M0 runs) and `container.txt` (cgroup peak, cap, OOM kills, link statistics,
health-check round-trip time).
