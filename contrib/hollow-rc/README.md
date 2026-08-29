# MVAPICH2 Hollow RC build and OSU runs

## One-command installation on a new machine

Install the Hollow RC kernel driver first, clone this repository under a
workspace directory, and run:

```bash
git clone --branch srm-cq-progress-poll \
  https://github.com/codingMIKUU/mvapich2-hollow.git
cd mvapich2-hollow
contrib/hollow-rc/bootstrap.sh
```

The bootstrap script installs missing Ubuntu build dependencies, clones the
matching `codingMIKUU/rdma-core` branch beside this repository, builds the
custom rdma-core in-place, and builds both ordinary and Hollow RC MVAPICH2.
Both MVAPICH2 variants use the bundled hwloc v2; the upstream v1 default
crashes during affinity setup on hosts with large CPU topologies.
Set `INSTALL_DEPS=0` to prohibit apt changes, `JOBS=N` to control build
parallelism, or `RDMA_CORE_ROOT`/`RDMA_CORE_REPO` to use an existing or private
rdma-core checkout.

Each MVAPICH installation receives a private copy of the matching libibverbs
and mlx5 provider under `lib/hollow-rc-rdma`. Runtime launch therefore does
not depend on Hydra's propagated `$HOME`, the source repository name, or an
external rdma-core build directory.

Build the selected transport on **both** machines. The helper derives all
paths from the source tree, so the account name is not embedded in the
result:

```bash
cd ~/zxm/mvapich2-2.3.7
contrib/hollow-rc/build_mvapich2.sh ordinary
contrib/hollow-rc/build_mvapich2.sh hollow
```

The defaults create separate installs:

- `~/zxm/mvapich2-2.3.7-install`
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
script uses `-m 1:1048576 -i 1000 -x 200 -f`. Both modes force the same
single-HCA, single-port, single-rail, SRQ and static-PMI setup so the ordinary
and Hollow RC measurements are directly comparable. Replace `hollow` with
`ordinary` to select the separately installed ordinary-RC build.

The rank wrapper defaults to RoCE v2 (`MV2_USE_ROCE_MODE=2`), port 1 and GID
index 3. Callers may override the RoCE mode, port or GID index explicitly.

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
