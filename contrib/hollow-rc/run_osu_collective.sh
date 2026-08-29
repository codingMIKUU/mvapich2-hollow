#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage:
  HOSTS=192.0.2.11,192.0.2.12 \
  HCA_MAP=192.0.2.11=mlx5_1,192.0.2.12=mlx5_3 \
    run_osu_collective.sh ordinary|hollow allreduce|alltoall [OSU options ...]

Optional environment:
  NP=16 PPN=8
  MVAPICH2_HOME=/explicit/local/install/prefix
  REMOTE_WORKSPACE=zxm
  RDMA_CORE_LIBDIR=/explicit/rdma-core/build/lib
EOF
    exit 2
}

mode=${1:-}
benchmark=${2:-}
case "$mode" in ordinary|hollow) ;; *) usage ;; esac
case "$benchmark" in allreduce|alltoall) ;; *) usage ;; esac
shift 2

hosts=${HOSTS:-}
hca_map=${HCA_MAP:-}
[[ -n "$hosts" && -n "$hca_map" ]] || usage

# Hydra accepts IP addresses in -hosts.  Check the mapping here so a missing
# per-machine HCA is reported before any ranks are launched.
IFS=',' read -r -a host_entries <<< "$hosts"
IFS=',' read -r -a hca_entries <<< "$hca_map"
for host_entry in "${host_entries[@]}"; do
    host_mapped=0
    for hca_entry in "${hca_entries[@]}"; do
        if [[ "${hca_entry%%=*}" == "$host_entry" &&
              "${hca_entry#*=}" != "$hca_entry" ]]; then
            host_mapped=1
            break
        fi
    done
    if (( !host_mapped )); then
        echo "No HCA mapping for HOSTS entry '$host_entry'." >&2
        exit 1
    fi
done

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd -- "$script_dir/../.." && pwd)
workspace_dir=$(dirname -- "$source_dir")
if [[ "$mode" == hollow ]]; then
    default_prefix="$workspace_dir/mvapich2-2.3.7-hollow-install"
    remote_install=mvapich2-2.3.7-hollow-install
else
    default_prefix="$workspace_dir/mvapich2-2.3.7-install"
    remote_install=mvapich2-2.3.7-install
fi

prefix=${MVAPICH2_HOME:-$default_prefix}
mpiexec="$prefix/bin/mpiexec"
[[ -x "$mpiexec" ]] || { echo "mpiexec not found: $mpiexec" >&2; exit 1; }

np=${NP:-16}
ppn=${PPN:-8}
remote_workspace=${REMOTE_WORKSPACE:-zxm}
remote_command='prefix="$HOME/'"$remote_workspace/$remote_install"'"; exec "$prefix/bin/mv2_hca_rank_wrapper.sh" "$@"'

if [[ $# -eq 0 ]]; then
    set -- -m 1:1048576 -i 1000 -x 200 -f
fi

exec "$mpiexec" \
    -hosts "$hosts" \
    -ppn "$ppn" \
    -n "$np" \
    -genv HOLLOW_RC_HCA_MAP "$hca_map" \
    -genv MV2_ENABLE_AFFINITY 1 \
    -genv MV2_CPU_BINDING_POLICY scatter \
    /bin/bash -lc "$remote_command" mv2-rank "$benchmark" "$@"
