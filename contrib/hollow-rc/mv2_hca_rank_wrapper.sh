#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 allreduce|alltoall [OSU options ...]" >&2
    exit 2
}

benchmark=${1:-}
case "$benchmark" in
    allreduce|alltoall) shift ;;
    *) usage ;;
esac

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
prefix=$(cd -- "$script_dir/.." && pwd)
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
elif [[ -r "$HOME/zxm/rdma-core/build-codex/lib/libibverbs.so" ]]; then
    rdma_lib="$HOME/zxm/rdma-core/build-codex/lib"
elif [[ -r "$HOME/zxm/rdma-core/build/lib/libibverbs.so" ]]; then
    rdma_lib="$HOME/zxm/rdma-core/build/lib"
else
    echo "Custom rdma-core library not found in build-codex/lib or build/lib." >&2
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
export MV2_USE_SRQ=1
export MV2_USE_RING_STARTUP=0
export MV2_USE_RDMA_CM=0
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

exec "$osu" "$@"
