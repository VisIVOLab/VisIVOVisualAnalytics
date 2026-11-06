| cube | way | moment | threads | range GB | n | wall s (median ± MAD) | first run s | GB/s | CPU/wall | anon peak GB | file peak GB | cap GB | status |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| syn450_beegfs | dask | M0 | 4 | 449.96 | 2 | 753 ± 3 | 775.9 | 0.60 | 2.5 | 10.20 | 244.18 | 16.0 | ok |
| syn450_beegfs | dask | M1 | 4 | 449.96 | 0 | n/a | - | n/a | n/a | 14.32 | 243.90 | 16.0 | MemoryError: Unable to allocate 141. MiB for an array with s; MemoryError: Unable to allocate 282. MiB for an array with s |
| syn450_beegfs | dask | M2 | 4 | 449.96 | 0 | n/a | - | n/a | n/a | 14.23 | 243.95 | 16.0 | MemoryError: Unable to allocate 282. MiB for an array with s |
