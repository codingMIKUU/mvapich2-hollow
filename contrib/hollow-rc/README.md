# MVAPICH2 Hollow RC build and OSU runs

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
from its own `$HOME`, which also avoids the former `lingbo11` versus `lingbo12`
path mix-up.

Single-machine example (replace the IP with this machine's reachable IP):

```bash
cd ~/zxm/mvapich2-2.3.7

LOCAL_IP=192.0.2.11
HOSTS=$LOCAL_IP \
HCA_MAP=$LOCAL_IP=mlx5_1 \
NP=16 PPN=16 \
contrib/hollow-rc/run_osu_collective.sh hollow allreduce

HOSTS=$LOCAL_IP \
HCA_MAP=$LOCAL_IP=mlx5_1 \
NP=16 PPN=16 \
contrib/hollow-rc/run_osu_collective.sh hollow alltoall
```

Two-machine example (replace both IP addresses with reachable addresses):

```bash
cd ~/zxm/mvapich2-2.3.7

HOSTS=192.0.2.11,192.0.2.12 \
HCA_MAP=192.0.2.11=mlx5_1,192.0.2.12=mlx5_3 \
NP=16 PPN=8 \
contrib/hollow-rc/run_osu_collective.sh hollow allreduce

HOSTS=192.0.2.11,192.0.2.12 \
HCA_MAP=192.0.2.11=mlx5_1,192.0.2.12=mlx5_3 \
NP=16 PPN=8 \
contrib/hollow-rc/run_osu_collective.sh hollow alltoall
```

Arguments after the benchmark name are passed to OSU. With no arguments the
script uses `-m 1:1048576 -i 1000 -x 200 -f`. Both modes force the same
single-HCA, single-port, single-rail, SRQ and static-PMI setup so the ordinary
and Hollow RC measurements are directly comparable. Replace `hollow` with
`ordinary` to select the separately installed ordinary-RC build.

The rank wrapper defaults `MV2_SMP_USE_CMA=0`.  This avoids MPI_Init failure
on machines whose Yama/container policy denies `process_vm_readv` between
local ranks.  This setting only selects the local shared-memory copy method;
it does not disable inter-node Hollow RC.  Set `MV2_SMP_USE_CMA=1` explicitly
only after CMA access has been enabled and verified on every machine.

The wrapper also sets `IBV_DRIVERS=mlx5`, so the custom mlx5 provider is
loaded even when the custom rdma-core was built with a non-existent
`/usr/local/etc/libibverbs.d` configuration directory.  A warning about that
directory is harmless; creating the directory merely silences the warning.
The wrapper first honors `RDMA_CORE_LIBDIR`, then automatically checks
`~/zxm/rdma-core/build-codex/lib` and `~/zxm/rdma-core/build/lib`, allowing
machines with either existing build-directory convention to use the same
installed MVAPICH package.
