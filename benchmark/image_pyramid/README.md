# Averaged tile pyramid of a 2-D image (issue #85)

`image_tile_bench.py` builds the averaged pyramid of a 2-D image with the
backend's own code (`app.brick_pyramid.build_pyramid`) and times the tiles of
`/v1/image/tile` (`app.compute.cube.worker_image_tile`) at every level, read
two ways: sampled from the file (every `2**L`-th pixel, the only path before
#85 and still the path while the pyramid builds) and from the pyramid
(averaged). With `--make WIDTHxHEIGHT` it first writes a synthetic image (unit
noise and Gaussian sources) row block by row block.

## Run

In a container whose memory is several times smaller than the image, as the
brick pyramid benchmark does (`benchmark/remote/Dockerfile` builds the image):

```bash
docker run --rm --memory 4g --memory-swap 4g --cpus 8 --shm-size 1g \
    -e PYTHONPATH=/app/backend \
    -v "$PWD/backend/app:/app/backend/app:ro" \
    -v "$PWD/benchmark/image_pyramid:/bench:ro" \
    -v ~/visivo_bench_data/img85:/work \
    visivo-bench-backend \
    python /bench/image_tile_bench.py --image /work/img_16gb.fits --make 65536x65536 \
        --out /work/img_16gb.pyr.h5 --json /work/img_16gb_mem4g.json
```

`--shm-size` gives the parallel build (issue #89) room for its shared buffers;
with Docker's default 64 MiB it builds with one process.

## Results

MacBook Pro (64 GB, 18 cores), Docker 29.8, the container limited to 4 GiB
of memory without swap and 8 CPUs; the image on the host's SSD through a bind
mount. `results/img_16gb_mem4g.json`.

- **Image:** 65536 × 65536 float32, 16.00 GiB, four times the container's
  memory.
- **Build:** 53 s with 8 processes; the pyramid is 21.34 GiB (4/3 of the image:
  each level is a quarter of the one below). The cgroup's peak equals its
  limit because page cache is charged to it; the build finishing under a
  limit four times smaller than the image is the evidence that it does not
  hold the image.
- **Tiles** (512 × 512, the size the desktop asks for; 30 random tiles per
  level and path):

| Level | Tiles in the level | Sampled from the file, median (p95) | Averaged from the pyramid, median (p95) |
|---|---|---|---|
| 0 | 16 384 | 113.0 ms (118.6) | 27.4 ms (32.6) |
| 1 | 4 096 | 113.1 ms (120.3) | 26.1 ms (27.3) |
| 2 | 1 024 | 109.9 ms (119.8) | 26.9 ms (28.8) |
| 3 | 256 | 98.5 ms (116.1) | 26.9 ms (28.0) |
| 4 | 64 | 91.2 ms (155.7) | 26.5 ms (28.5) |
| 5 | 16 | 94.8 ms (135.6) | 25.8 ms (28.3) |
| 6 | 4 | 97.8 ms (107.8) | 26.6 ms (28.0) |
| 7 | 1 | 112.4 ms (125.3) | 26.0 ms (28.0) |

A tile from the pyramid costs the same at every level, about 26 ms, most of it
the zlib compression of the reply; a tile sampled from the file costs about
four times as much, at every level too: a coarse tile touches fewer pixels but
as many pages, spread across the file. The desktop fetches up to eight tiles
at once, so a view of 4 × 3 tiles arrives in about two rounds.
