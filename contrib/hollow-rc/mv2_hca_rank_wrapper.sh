#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 allreduce|alltoall|bcast [OSU options ...] (broadcast is an alias for bcast)" >&2
    exit 2
}

benchmark=${1:-}
case "$benchmark" in
    broadcast) benchmark=bcast; shift ;;
    allreduce|alltoall|bcast) shift ;;
    *) usage ;;
esac

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
prefix=$(cd -- "$script_dir/.." && pwd)
workspace_dir=$(dirname -- "$prefix")
bundled_rdma_lib="$prefix/lib/hollow-rc-rdma"
rdma_config="$prefix/etc/hollow-rc-rdma-libdir"
host=$(hostname -s)
host_full=$(hostname -f 2>/dev/null || true)
local_ipv4=$(hostname -I 2>/dev/null || true)
hca=

# Format: host-or-ip=mlx5_N,host-or-ip=mlx5_M
IFS=',' read -r -a hca_entries <<< "${HOLLOW_RC_HCA_MAP:-}"
for entry in "${hca_entries[@]}"; do
    key=${entry%%=*}
    value=${entry#*=}
    key_matches=0
    if [[ -n "$key" && "$value" != "$entry" ]]; then
        if [[ "$host" == "$key" || "$host_full" == "$key" ]]; then
            key_matches=1
        else
            for address in $local_ipv4; do
                if [[ "$address" == "$key" ]]; then
                    key_matches=1
                    break
                fi
            done
        fi
    fi
    if (( key_matches )); then
        hca=$value
        break
    fi
done

if [[ -z "$hca" ]]; then
    echo "No HCA mapping for host '$host' (local IPv4: $local_ipv4)." >&2
    echo "Set HOLLOW_RC_HCA_MAP, for example:" >&2
    echo "  192.0.2.11=mlx5_1,192.0.2.12=mlx5_3" >&2
    exit 1
fi

if [[ -n "${RDMA_CORE_LIBDIR:-}" ]]; then
    rdma_lib=$RDMA_CORE_LIBDIR
elif [[ -r "$bundled_rdma_lib/libibverbs.so" ]]; then
    rdma_lib=$bundled_rdma_lib
elif [[ -r "$rdma_config" ]]; then
    IFS= read -r rdma_lib < "$rdma_config"
elif [[ -r "$workspace_dir/rdma-core/build-codex/lib/libibverbs.so" ]]; then
    rdma_lib="$workspace_dir/rdma-core/build-codex/lib"
elif [[ -r "$workspace_dir/rdma-core/build/lib/libibverbs.so" ]]; then
    rdma_lib="$workspace_dir/rdma-core/build/lib"
else
    echo "Custom rdma-core library not found for MVAPICH prefix: $prefix" >&2
    echo "Set RDMA_CORE_LIBDIR to the directory containing libibverbs.so." >&2
    exit 1
fi
if [[ ! -r "$rdma_lib/libibverbs.so" ]]; then
    echo "Custom rdma-core library not found: $rdma_lib" >&2
    exit 1
fi

export MV2_IBA_HCA=$hca
export MV2_NUM_HCAS=1
export MV2_NUM_PORTS=1
export MV2_NUM_QP_PER_PORT=1
export MV2_DEFAULT_PORT=${MV2_DEFAULT_PORT:-1}
export MV2_DEFAULT_GID_INDEX=${MV2_DEFAULT_GID_INDEX:-3}
export MV2_USE_ROCE_MODE=${MV2_USE_ROCE_MODE:-2}
export MV2_USE_SRQ=1
export MV2_USE_RING_STARTUP=0
export MV2_USE_RDMA_CM=0
case "${MV2_TRANSPORT_MODE:-ordinary}" in
    xrc)
        export MV2_USE_XRC=1
        ;;
    ordinary|hollow)
        export MV2_USE_XRC=0
        ;;
    *)
        echo "Unknown MV2 transport mode: $MV2_TRANSPORT_MODE" >&2
        exit 1
        ;;
esac
# Use the same eager, all-to-all connection setup for ordinary and Hollow RC.
# INT_MAX keeps every practical job below the on-demand threshold, while the
# explicit UD settings prevent the separate UD/RC hybrid data path from being
# selected.  Basic CM changes only connection setup; inter-node payload still
# uses the transport selected by this installation (ordinary or Hollow RC).
export MV2_ON_DEMAND_THRESHOLD=2147483647
export MV2_USE_UD_HYBRID=0
export MV2_USE_ONLY_UD=0

# Keep each rank's CPU and memory on the NUMA node local to its selected HCA.
# This is evaluated independently on every host, so heterogeneous HCA names
# and CPU numberings do not require a launcher-side CPU map.  An explicit
# MV2_CPU_MAPPING remains available for experiments that need a custom map.
hca_device=$(readlink -f "/sys/class/infiniband/$hca/device")
if [[ ! -r "$hca_device/numa_node" ]]; then
    echo "Cannot determine NUMA node for HCA '$hca'." >&2
    exit 1
fi
hca_numa_node=$(<"$hca_device/numa_node")
if (( hca_numa_node < 0 )); then
    echo "HCA '$hca' does not report a usable NUMA node." >&2
    exit 1
fi

if [[ -z "${MV2_CPU_MAPPING:-}" ]]; then
    # CPU 174 runs the single Hollow scheduler and CPU 175 runs its server
    # thread on the current two hosts.  Keep both CPUs out of the MPI rank map
    # in ordinary and Hollow runs so their CPU layouts remain comparable.
    reserved_cpus=",174,175,"
    cpu_mapping=$(lscpu -p=CPU,NODE | awk -F, -v node="$hca_numa_node" \
        -v reserved="$reserved_cpus" '
        $1 !~ /^#/ && $2 == node && index(reserved, "," $1 ",") == 0 {
            if (mapping != "") mapping = mapping ":";
            mapping = mapping $1;
        }
        END { print mapping }
    ')
    if [[ -z "$cpu_mapping" ]]; then
        echo "No online CPUs found on NUMA node $hca_numa_node for HCA '$hca'." >&2
        exit 1
    fi
    export MV2_CPU_MAPPING=$cpu_mapping
fi

if ! command -v numactl >/dev/null 2>&1; then
    echo "numactl is required to bind rank memory to NUMA node $hca_numa_node." >&2
    exit 1
fi
# CMA uses process_vm_readv/process_vm_writev between local ranks.  Some
# systems prohibit that through Yama/container policy; use the portable
# shared-memory copy path by default.  A caller may explicitly opt back in.
export MV2_SMP_USE_CMA=${MV2_SMP_USE_CMA:-0}
export LD_LIBRARY_PATH="$rdma_lib:$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export IBV_DRIVERS_PATH=${IBV_DRIVERS_PATH:-$rdma_lib}
# This rdma-core generation uses IBV_DRIVERS/RDMAV_DRIVERS to select a
# provider when its compiled-in libibverbs.d directory is unavailable.
export IBV_DRIVERS=${IBV_DRIVERS:-mlx5}

osu="$prefix/libexec/osu-micro-benchmarks/mpi/collective/osu_$benchmark"
if [[ ! -x "$osu" ]]; then
    echo "OSU benchmark not installed: $osu" >&2
    exit 1
fi

exec numactl --membind="$hca_numa_node" "$osu" "$@"
