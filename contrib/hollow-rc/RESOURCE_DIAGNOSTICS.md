# Mrail resource-pressure diagnostics

This observes RC/XRC/Hollow transport resource events. It does not change
Alltoall algorithms, Eager thresholds, resource sizes, retry behaviour or
collective synchronization. In particular, fixed Pairwise has no separately
selected intra-node Alltoall algorithm. Local point-to-point communication
still uses SMP Eager/Rendezvous and its own resource pools.

## Controls

* `MV2_RESOURCE_LOG=1`: enable; default `0`.
* `MV2_RESOURCE_LOG_RANK=N`: only report global rank N; default `-1` reports all.
* `MV2_RESOURCE_LOG_MAX=8`: maximum live lines per event type per rank (1-64).
  Live events are reported at occurrence counts 1, 2, 4, 8, ...; with default
  8 the last reported occurrence is 128. Suppressed events are still counted.
* `CPPFLAGS=-DMV2_ENABLE_RESOURCE_LOG=0` at build time removes the logger.

The disabled path does not count events, read clocks, or evaluate log
arguments. It retains predictable branches/small event-detection bookkeeping;
it is not claimed to be instruction-for-instruction identical. Enabled event
counts use atomic increments shared with the async SRQ thread. Formatting,
timestamps and a single bounded stderr write occur only on selected events.
There are no per-WQE clocks, new MPI operations or extra resource locks.
Logging can perturb timing, especially when all ranks print, and a slow stderr
consumer can block. First diagnose one rank or use short all-rank runs, then
turn logging off for performance measurements. Some event sites already hold
resource locks; enabling output is not a performance-neutral operation.

## Events (MV2_RESOURCE_EVENT)

| event | meaning | not a claim of |
|---|---|---|
| `srq_low_water` | RC/XRC SRQ low-water event; `fill_before`, `fill_after`, `limit`, `capacity` are software targets; `posted` is the actual refill count | SRQ is empty, actual receives remaining equal `limit`, or RNR occurred |
| `srq_post_short` | requested receives were not all posted; includes `requested`, `posted`, `reason`, verbs `ret`, saved `errno` | successful full refill |
| `srq_refill_wait` | async event handler reaches its existing zero-refill wait path; `rc_srq=0` identifies UD | duration spent blocked (not timed) |
| `vbuf_grow` | empty vbuf free list causes pool expansion; includes pool, buffer bytes, prior allocated count and grow increment | system RAM exhausted or allocation failed |
| `send_wqe_empty` | a vbuf is queued while the logical rail has zero send WQE credits | actual hardware CQ overflow or Hollow shared-SQ occupancy |
| `forced_rndv` | extended send queue crosses its limit and a VC transitions to `force_rndv=1` | collective algorithm change |

The normal code can force Rendezvous below the configured Eager byte threshold
when the extended send queue grows. This log records that transition, not
every subsequent message that uses Rendezvous. Clearing `force_rndv` remains
unchanged and is not logged. `count` is per event type per rank, aggregated
across HCAs/rails/pools; per-line fields describe the selected occurrence.
Send-queue events include `sge_bytes`, the queued vbuf's SGE length. This may
include a transport header or describe a control packet; it is not necessarily
the OSU message size.
`since_init_ms` is local elapsed time since diagnostic initialization, not a
clock synchronized between hosts. `vbuf_grow` includes initialization-time
growth and only the two get_vbuf entry points, not every allocator in MVAPICH.

Normal process exit prints `MV2_RESOURCE_SUMMARY` with totals, including
suppressed occurrences. Abort/SIGKILL can lose summaries. No event output does
not rule out SMP queue pressure, fabric congestion, recoverable hardware RNR,
or Hollow kernel/shared credit waits: these are outside this instrumentation.

## Run / build

Use your unchanged launcher command with these extra environment variables:

```bash
HOSTS=192.168.1.5,192.168.1.1,192.168.1.2 \
HCA_MAP=192.168.1.5=mlx5_1,192.168.1.1=mlx5_3,192.168.1.2=mlx5_1 \
USER_MAP=192.168.1.5=lingbo11,192.168.1.1=lingbo12,192.168.1.2=lingbo10 \
NP=384 PPN=128 \
MV2_IBA_EAGER_THRESHOLD=131072 MV2_ALLTOALL_TUNING=3 \
MV2_USE_XOR_ALLTOALL=0 MV2_RNDV_PROTOCOL=RPUT \
MV2_RESOURCE_LOG=1 MV2_RESOURCE_LOG_RANK=0 \
contrib/hollow-rc/run_osu_collective.sh ordinary alltoall \
-m 1024:262144 -i 100 -x 50 -f 2>resource-ordinary.log
```

Set RANK to `-1` for all ranks if rank 0 sees no pressure. These are live
resource events, not per-message-size statistics. Normal OSU stdout still
prints after each size; use combined stdout/stderr if chronological size
boundaries are useful, noting inter-rank buffering can reorder observations.

Only MPI libmpi needs rebuilding. After all jobs using an installation have
exited, use the existing build script on each participating machine:

```bash
JOBS=4 contrib/hollow-rc/build_mvapich2.sh ordinary  # also contains XRC
JOBS=4 contrib/hollow-rc/build_mvapich2.sh hollow
```

These scripts configure/build/install MPI and bundle the existing rdma-core
binaries. They do not rebuild rdma-core or reload the kernel. Already correctly
configured build trees can instead incrementally build `lib/libmpi.la` and
`install-libLTLIBRARIES` as documented in ALLREDUCE_PHASE_DIAGNOSTICS.md.
Do not install over libraries in use by an active benchmark.

Offline test (no MPI launch or RDMA hardware):

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/hollow-rc/tests -p test_resource_log.py -v
```
