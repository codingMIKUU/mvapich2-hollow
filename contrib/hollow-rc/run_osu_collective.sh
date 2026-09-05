#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage:
  HOSTS=192.0.2.11,192.0.2.12 \
  HCA_MAP=192.0.2.11=mlx5_1,192.0.2.12=mlx5_3 \
  USER_MAP=192.0.2.11=user1,192.0.2.12=user2 \
    run_osu_collective.sh ordinary|xrc|hollow allreduce|alltoall [OSU options ...]

Optional environment:
  NP=16 PPN=8
  MVAPICH2_HOME=/explicit/local/install/prefix
  REMOTE_WORKSPACE=zxm
  RDMA_CORE_LIBDIR=/explicit/rdma-core/build/lib
  RUN_WDIR=/directory/available/on/all/hosts
  ALLREDUCE_PROFILE=auto         # Keep MVAPICH defaults/native MV2 overrides
    # Or: flat-rd, flat-rsag, twolevel-rd-shmem, twolevel-rsag-shmem,
    #     twolevel-rd-p2p. Fixed profiles are identical for all transports.
  DRY_RUN=1                     # Print launch arguments; do not start MPI/SSH
  MV2_MEMORY_OPTIMIZATION=1       # Hollow default; explicit values win
  MV2_SRQ_SIZE=<initial receives> # Hollow default: power-of-two >= NP, min 256
  MV2_SRQ_LIMIT=<low watermark>   # Hollow default: one quarter of SRQ_SIZE
  MV2_SRQ_MAX_SIZE=<maximum>      # Hollow default: 8192
EOF
    exit 2
}

mode=${1:-}
benchmark=${2:-}
case "$mode" in ordinary|xrc|hollow) ;; *) usage ;; esac
case "$benchmark" in allreduce|alltoall) ;; *) usage ;; esac
shift 2

# Apply only at launch, never in the MPI/verbs data path. Explicit -genv
# arguments carry the same settings to every rank, including remote accounts.
# Native MV2 overrides remain available with the default profile, "auto".
allreduce_profile=${ALLREDUCE_PROFILE:-auto}
allreduce_args=()
allreduce_settings=()
dry_run=${DRY_RUN:-0}
if [[ "$dry_run" != 0 && "$dry_run" != 1 ]]; then
    echo "DRY_RUN must be 0 or 1." >&2
    exit 1
fi
if [[ "$benchmark" != allreduce && "$allreduce_profile" != auto ]]; then
    echo "ALLREDUCE_PROFILE applies only to allreduce, not $benchmark." >&2
    exit 1
fi
if [[ "$benchmark" == allreduce && "$allreduce_profile" != auto ]]; then
    allreduce_intra=5
    case "$allreduce_profile" in
        flat-rd)              allreduce_inter=1; allreduce_two_level=0 ;;
        flat-rsag)            allreduce_inter=2; allreduce_two_level=0 ;;
        twolevel-rd-shmem)    allreduce_inter=1; allreduce_two_level=1 ;;
        twolevel-rsag-shmem)  allreduce_inter=2; allreduce_two_level=1 ;;
        twolevel-rd-p2p)      allreduce_inter=1; allreduce_two_level=1; allreduce_intra=6 ;;
        *)
            echo "Unknown ALLREDUCE_PROFILE '$allreduce_profile'. See usage in this script." >&2
            exit 1
            ;;
    esac
    allreduce_settings=(
        "MV2_INTER_ALLREDUCE_TUNING=$allreduce_inter"
        "MV2_INTER_ALLREDUCE_TUNING_TWO_LEVEL=$allreduce_two_level"
        "MV2_INTRA_ALLREDUCE_TUNING=$allreduce_intra"
        MV2_USE_OSU_COLLECTIVES=1
        MV2_USE_ANL_COLLECTIVES=0
        MV2_USE_OLD_ALLREDUCE=0
        MV2_USE_INDEXED_ALLREDUCE_TUNING=1
        MV2_USE_SHARED_MEM=1
        MV2_USE_SHMEM_COLL=1
        MV2_USE_SHMEM_ALLREDUCE=1
        MV2_USE_BLOCKING=0
        MV2_ENABLE_ALLREDUCE_SKIP_SMALL_MESSAGE_TUNING_TABLE_SEARCH=0
        MV2_ENABLE_ALLREDUCE_SKIP_LARGE_MESSAGE_TUNING_TABLE_SEARCH=0
        MV2_USE_MCAST=0
        MV2_ENABLE_SHARP=0
        # MPI_T uses a separate numbering scheme and conflicts with MV2_INTER.
        # Set all aliases to their "not selected" value, also preventing a
        # per-host config file from supplying a conflicting CVAR selection.
        MPICH_ALLREDUCE_COLLECTIVE_ALGORITHM=-1
        MV2_ALLREDUCE_COLLECTIVE_ALGORITHM=-1
        MPIR_PARAM_ALLREDUCE_COLLECTIVE_ALGORITHM=-1
        MPIR_CVAR_ALLREDUCE_COLLECTIVE_ALGORITHM=-1
    )
    for allreduce_setting in "${allreduce_settings[@]}"; do
        allreduce_name=${allreduce_setting%%=*}
        allreduce_value=${allreduce_setting#*=}
        if [[ -v "$allreduce_name" && "${!allreduce_name}" != "$allreduce_value" ]]; then
            echo "ALLREDUCE_PROFILE=$allreduce_profile conflicts with $allreduce_name=${!allreduce_name} (requires $allreduce_value)." >&2
            echo "Unset the conflicting variable, or use ALLREDUCE_PROFILE=auto for native MV2 tuning." >&2
            exit 1
        fi
        allreduce_args+=(-genv "$allreduce_name" "$allreduce_value")
    done
fi

hosts=${HOSTS:-}
hca_map=${HCA_MAP:-}
user_map=${USER_MAP:-}
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

hostfile=
if [[ -n "$user_map" ]]; then
    IFS=',' read -r -a user_entries <<< "$user_map"
    hostfile=$(mktemp "${TMPDIR:-/tmp}/mv2-hollow-hosts.XXXXXX")
    trap 'rm -f -- "$hostfile"' EXIT
    for host_entry in "${host_entries[@]}"; do
        host_user=
        for user_entry in "${user_entries[@]}"; do
            if [[ "${user_entry%%=*}" == "$host_entry" &&
                  "${user_entry#*=}" != "$user_entry" ]]; then
                host_user=${user_entry#*=}
                break
            fi
        done
        if [[ -z "$host_user" ]]; then
            echo "No user mapping for HOSTS entry '$host_entry'." >&2
            exit 1
        fi
        printf '%s:%s user=%s\n' "$host_entry" "${PPN:-8}" "$host_user" >> "$hostfile"
    done
fi

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
run_wdir=${RUN_WDIR:-/tmp}
remote_command='account_home=$(getent passwd "$(id -u)" | cut -d: -f6); for prefix in "$account_home/'"$remote_workspace/$remote_install"'" "$account_home/'"$remote_install"'"; do if [[ -x "$prefix/bin/mv2_hca_rank_wrapper.sh" ]]; then exec "$prefix/bin/mv2_hca_rank_wrapper.sh" "$@"; fi; done; echo "MVAPICH install not found below $account_home/'"$remote_workspace"' or $account_home" >&2; exit 1'

hollow_srq_args=()
if [[ "$mode" == hollow ]]; then
    for numeric_value in "$np" "${MV2_SRQ_SIZE:-256}" \
                         "${MV2_SRQ_LIMIT:-30}" \
                         "${MV2_SRQ_MAX_SIZE:-8192}"; do
        if [[ ! "$numeric_value" =~ ^[1-9][0-9]*$ ]]; then
            echo "Hollow SRQ values and NP must be positive integers." >&2
            exit 1
        fi
    done

    # One eager SEND consumes one Receive WQE.  Size the initial per-rank SRQ
    # for one worst-case all-to-all fan-in instead of MVAPICH2's memory-saving
    # default of 80 entries.  Round up for stable queue geometry and retain a
    # minimum suitable for smaller jobs.
    auto_srq_size=256
    while (( auto_srq_size < np )); do
        auto_srq_size=$((auto_srq_size * 2))
    done

    # Keep MVAPICH2's memory-saving vbuf policy by default.  The receive pool
    # grows in secondary batches when SRQ_SIZE exceeds the initial vbuf pool,
    # so disabling memory optimization is unnecessary and can exhaust the
    # per-node HugeTLB pool when many local ranks start simultaneously.
    hollow_memory_optimization=${MV2_MEMORY_OPTIMIZATION:-1}
    hollow_srq_size=${MV2_SRQ_SIZE:-$auto_srq_size}
    hollow_srq_limit=${MV2_SRQ_LIMIT:-$((hollow_srq_size / 4))}
    hollow_srq_max_size=${MV2_SRQ_MAX_SIZE:-8192}

    if [[ ! "$hollow_memory_optimization" =~ ^[01]$ ]]; then
        echo "MV2_MEMORY_OPTIMIZATION must be 0 or 1." >&2
        exit 1
    fi
    if (( hollow_srq_limit >= hollow_srq_size )); then
        echo "MV2_SRQ_LIMIT must be smaller than MV2_SRQ_SIZE." >&2
        exit 1
    fi
    if (( hollow_srq_size > hollow_srq_max_size )); then
        echo "MV2_SRQ_SIZE must not exceed MV2_SRQ_MAX_SIZE." >&2
        exit 1
    fi

    hollow_srq_args=(
        -genv MV2_MEMORY_OPTIMIZATION "$hollow_memory_optimization"
        -genv MV2_SRQ_SIZE "$hollow_srq_size"
        -genv MV2_SRQ_LIMIT "$hollow_srq_limit"
        -genv MV2_SRQ_MAX_SIZE "$hollow_srq_max_size"
    )
    echo "Hollow SRQ initial=$hollow_srq_size limit=$hollow_srq_limit max=$hollow_srq_max_size memory_optimization=$hollow_memory_optimization"
fi

if [[ $# -eq 0 ]]; then
    set -- -m 1:1048576 -i 1000 -x 200 -f
fi

host_args=(-hosts "$hosts")
if [[ -n "$hostfile" ]]; then
    host_args=(-f "$hostfile")
fi

if [[ "$benchmark" == allreduce ]]; then
    if [[ "$allreduce_profile" == auto ]]; then
        echo "ALLREDUCE_CONFIG scope=requested transport=$mode profile=auto selection=MVAPICH-default-or-native-MV2-overrides np=$np ppn=$ppn"
    else
        allreduce_intra_label=$allreduce_intra
        if (( allreduce_two_level == 0 )); then
            allreduce_intra_label=unused-flat
        fi
        echo "ALLREDUCE_CONFIG scope=requested transport=$mode profile=$allreduce_profile inter=$allreduce_inter two_level=$allreduce_two_level intra=$allreduce_intra_label np=$np ppn=$ppn"
    fi
fi

launch_args=("$mpiexec"
    "${host_args[@]}" \
    -launcher-exec "$prefix/bin/mv2_hydra_ssh_wrapper.sh" \
    -wdir "$run_wdir" \
    -ppn "$ppn" \
    -n "$np" \
    -genv HOLLOW_RC_HCA_MAP "$hca_map" \
    -genv MV2_TRANSPORT_MODE "$mode" \
    -genv MV2_ON_DEMAND_THRESHOLD 2147483647 \
    -genv MV2_USE_UD_HYBRID 0 \
    -genv MV2_USE_ONLY_UD 0 \
    -genv MV2_ENABLE_AFFINITY "${MV2_ENABLE_AFFINITY:-1}" \
    -genv MV2_CPU_BINDING_POLICY "${MV2_CPU_BINDING_POLICY:-scatter}" \
    "${hollow_srq_args[@]}" \
    "${allreduce_args[@]}" \
    /bin/bash -lc "$remote_command" mv2-rank "$benchmark" "$@")

if [[ "$dry_run" == 1 ]]; then
    # This is an inspection output, not a replay script: a temporary hostfile
    # (when USER_MAP is supplied) is removed by the normal EXIT trap.
    printf 'DRY_RUN hosts=%s user_map=%s\n' "$hosts" "$user_map"
    printf 'MV2_REMOTE_WORKSPACE=%q MV2_REMOTE_INSTALL=%q ' "$remote_workspace" "$remote_install"
    printf '%q ' "${launch_args[@]}"
    printf '\n'
    exit 0
fi

MV2_REMOTE_WORKSPACE="$remote_workspace" \
MV2_REMOTE_INSTALL="$remote_install" \
"${launch_args[@]}"
