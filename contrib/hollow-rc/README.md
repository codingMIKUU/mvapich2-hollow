# MVAPICH2 Hollow RC build and OSU runs

## One-command installation on a new machine

Install the Hollow RC kernel driver first, clone this repository under a
workspace directory, and run:

```bash
mkdir -p ~/zxm
cd ~/zxm
git clone --branch srm-cq-progress-poll \
  https://github.com/codingMIKUU/mvapich2-hollow.git \
  mvapich2-2.3.7
cd mvapich2-2.3.7
JOBS="$(nproc)" ./install_hollow_rc.sh
```

`install_hollow_rc.sh` is the public one-command entry point. It invokes
`contrib/hollow-rc/bootstrap.sh`, which performs and verifies all five stages:

1. Install missing Ubuntu build dependencies.
2. Clone the matching `codingMIKUU/rdma-core` branch beside this repository.
3. Configure and build custom rdma-core in `~/zxm/rdma-core/build-codex`.
4. Configure, build and install ordinary/XRC MVAPICH2 with
   `--disable-hollow-rc --enable-xrc` into
   `~/zxm/mvapich2-2.3.7-install`.
5. Configure, build and install Hollow RC MVAPICH2 with
   `--enable-hollow-rc` into `~/zxm/mvapich2-2.3.7-hollow-install`.

Stages 4 and 5 both invoke `contrib/hollow-rc/build_mvapich2.sh`. That helper
passes the custom rdma-core include and library directories to `configure`,
runs parallel `make` and `make install`, installs the Hydra/HCA wrappers, and
copies `libibverbs`, `libmlx5`, `librdmacm` and the mlx5 provider into each
installation's `lib/hollow-rc-rdma` directory. The bootstrap finally checks
both MPI/OSU executables and compares the bundled RDMA libraries with the
fresh build before reporting success.

Both MVAPICH2 variants use the bundled hwloc v2; the upstream v1 default
crashes during affinity setup on hosts with large CPU topologies.
Set `INSTALL_DEPS=0` to prohibit apt changes, `JOBS=N` to control build
parallelism, or `RDMA_CORE_ROOT`/`RDMA_CORE_REPO` to use an existing or private
rdma-core checkout.

Each MVAPICH installation receives a private copy of the matching libibverbs
and mlx5 provider under `lib/hollow-rc-rdma`. Runtime launch therefore does
not depend on Hydra's propagated `$HOME`, the source repository name, or an
external rdma-core build directory.

If dependencies are already installed, the same single entry point is:

```bash
INSTALL_DEPS=0 JOBS="$(nproc)" ./install_hollow_rc.sh
```

## Rebuild only rdma-core after a userspace-provider change

When only the custom rdma-core implementation changes (for example,
`providers/mlx5/qp.c` or `providers/mlx5/cq.c`), stop all OSU/Hydra processes
and run:

```bash
cd ~/zxm/mvapich2-2.3.7
contrib/hollow-rc/rebuild_rdma_core.sh
```

The helper incrementally rebuilds the sibling `rdma-core` checkout in
`build-codex`, copies libibverbs, librdmacm and the mlx5 provider into both
the ordinary and Hollow RC MVAPICH installation prefixes, and verifies the
copied libraries.  It does not rebuild MVAPICH2 or install anything under
`/usr` or `/usr/local`; existing `run_osu_collective.sh` commands can be used
immediately afterward.

If the rdma-core change modifies public headers, structure layouts, exported
symbols or another ABI consumed while compiling MVAPICH2, use the same helper
in full userspace-rebuild mode:

```bash
cd ~/zxm/mvapich2-2.3.7
REBUILD_MVAPICH=1 contrib/hollow-rc/rebuild_rdma_core.sh
```

Set `JOBS=N`, `RDMA_CORE_ROOT=/path/to/rdma-core`, or
`RDMA_CORE_BUILD=/path/to/build` when the defaults are unsuitable.  A new
machine should still use `contrib/hollow-rc/bootstrap.sh`; that script also
installs dependencies and creates both MVAPICH installations from scratch.

Hydra normally launches its remote proxy using the launcher's absolute
installation path, which fails when machines use different account names.
The installed `mv2_hydra_ssh_wrapper.sh` resolves each remote account's real
home directory during job startup and launches that host's matching proxy.
This adds one SSH lookup per remote host at startup and no data-path overhead.

Build the selected transport on **both** machines. The helper derives all
paths from the source tree, so the account name is not embedded in the
result:

```bash
cd ~/zxm/mvapich2-2.3.7
contrib/hollow-rc/build_mvapich2.sh ordinary
contrib/hollow-rc/build_mvapich2.sh hollow
```

The defaults create two installs. Ordinary RC and XRC share the first binary
and are selected at launch; Hollow RC uses the second binary:

- `~/zxm/mvapich2-2.3.7-install` (ordinary RC and XRC)
- `~/zxm/mvapich2-2.3.7-hollow-install`

Run from either machine. Use reachable management/RDMA-network IP addresses;
host-name resolution is not required. Each rank derives its install prefix
from the home directory associated with its actual UID instead of trusting
Hydra's propagated `$HOME`. This avoids the former `lingbo11` versus
`lingbo12` path mix-up.

The launcher uses `/tmp` as the default process working directory because
the two accounts have different absolute home-directory paths. Set
`RUN_WDIR` only to a path that exists on every participating machine.

Single-machine example (replace the IP with this machine's reachable IP):

```bash
cd ~/zxm/mvapich2-2.3.7

LOCAL_IP=192.168.1.5
HOSTS=$LOCAL_IP \
HCA_MAP=$LOCAL_IP=mlx5_1 \
USER_MAP=$LOCAL_IP=lingbo11 \
NP=16 PPN=16 \
contrib/hollow-rc/run_osu_collective.sh hollow allreduce

HOSTS=$LOCAL_IP \
HCA_MAP=$LOCAL_IP=mlx5_1 \
USER_MAP=$LOCAL_IP=lingbo11 \
NP=16 PPN=16 \
contrib/hollow-rc/run_osu_collective.sh hollow alltoall
```

Two-machine example (replace both IP addresses with reachable addresses):

```bash
cd ~/zxm/mvapich2-2.3.7

HOSTS=192.168.1.5,192.168.1.1 \
HCA_MAP=192.168.1.5=mlx5_1,192.168.1.1=mlx5_3 \
USER_MAP=192.168.1.5=lingbo11,192.168.1.1=lingbo12 \
NP=16 PPN=8 \
contrib/hollow-rc/run_osu_collective.sh hollow allreduce

HOSTS=192.168.1.5,192.168.1.1 \
HCA_MAP=192.168.1.5=mlx5_1,192.168.1.1=mlx5_3 \
USER_MAP=192.168.1.5=lingbo11,192.168.1.1=lingbo12 \
NP=16 PPN=8 \
contrib/hollow-rc/run_osu_collective.sh hollow alltoall
```

Arguments after the benchmark name are passed to OSU. With no arguments the
script uses `-m 1:1048576 -i 1000 -x 200 -f`. All three modes force the same
single-HCA, single-port, single-rail, SRQ and static-PMI setup so the ordinary
RC, XRC and Hollow RC measurements are directly comparable. Use `ordinary`
or `xrc` to select the corresponding runtime path in the ordinary/XRC build,
or `hollow` to select the separate Hollow binary. The rank wrapper translates
the mode into `MV2_USE_XRC=0/1`; no source edit or reinstall is required when
switching between ordinary RC and XRC.

For example, a two-machine XRC run is:

```bash
HOSTS=192.168.1.5,192.168.1.1 \
HCA_MAP=192.168.1.5=mlx5_1,192.168.1.1=mlx5_3 \
USER_MAP=192.168.1.5=lingbo11,192.168.1.1=lingbo12 \
NP=16 PPN=8 \
contrib/hollow-rc/run_osu_collective.sh xrc allreduce \
  -m 4:1048576 -i 1000 -x 200 -f
```

At startup rank 0 prints `MV2 transport=xrc` when the XRC path is active.
XRC uses the modern rdma-core XRCD, XRC SRQ, XRC send-QP and receive-QP APIs;
the previous removed `ibv_open_xrc_domain` API is not required.

Both installed modes also use Basic all-to-all connection management.  The
rank wrapper fixes `MV2_ON_DEMAND_THRESHOLD=2147483647` and disables the UD
hybrid/UD-only data paths, so large jobs do not silently switch to on-demand
UD connection management.  Ordinary mode therefore pre-creates ordinary RC
connections, while Hollow mode pre-creates logical Hollow RC connections.
This avoids the oversized on-demand `cm_msg` on 1024-byte RoCE paths and keeps
the connection-management policy identical in comparison runs.  Be aware
that ordinary RC now pays the full QP and pinned-memory cost at large rank
counts.

The rank wrapper defaults to RoCE v2 (`MV2_USE_ROCE_MODE=2`), port 1 and GID
index 3. Callers may override the RoCE mode, port or GID index explicitly.

Each host reads the NUMA node of its selected HCA, builds an
`MV2_CPU_MAPPING` from the online CPUs on that node, and starts the benchmark
under `numactl --membind` for the same node.  For the current hosts this binds
`mlx5_1` on lingbo11 and `mlx5_3` on lingbo12 to NUMA node 1 (CPUs 144-287).
The generated mapping excludes CPU 174, which runs the Hollow scheduler, and
CPU 175, which runs the Hollow connection server.  With `PPN=128`, ranks use
CPUs 144-173 and 176-273 on each host.
An explicitly supplied `MV2_CPU_MAPPING` overrides only the generated CPU
order; rank memory remains local to the selected HCA's NUMA node.

The rank wrapper defaults `MV2_SMP_USE_CMA=0`.  This avoids MPI_Init failure
on machines whose Yama/container policy denies `process_vm_readv` between
local ranks.  This setting only selects the local shared-memory copy method;
it does not disable inter-node Hollow RC.  Set `MV2_SMP_USE_CMA=1` explicitly
only after CMA access has been enabled and verified on every machine.

The wrapper also sets `IBV_DRIVERS=mlx5`, so the custom mlx5 provider is
loaded even when no system-wide `libibverbs.d` configuration is installed.
The wrapper first honors `RDMA_CORE_LIBDIR`, then derives the workspace from
its own MVAPICH install prefix and checks `rdma-core/build-codex/lib` followed
by `rdma-core/build/lib`. This avoids trusting a `$HOME` value propagated from
another account while allowing both existing build-directory conventions.
