#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd -- "$script_dir/../.." && pwd)
workspace_dir=$(dirname -- "$source_dir")

jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
rdma_core_root=${RDMA_CORE_ROOT:-"$workspace_dir/rdma-core"}
rdma_core_build=${RDMA_CORE_BUILD:-"$rdma_core_root/build-codex"}
rebuild_mvapich=${REBUILD_MVAPICH:-0}

case "$rebuild_mvapich" in
    0|1) ;;
    *)
        echo "REBUILD_MVAPICH must be 0 or 1." >&2
        exit 2
        ;;
esac

if [[ ! -f "$rdma_core_root/CMakeLists.txt" ]]; then
    echo "rdma-core source tree not found: $rdma_core_root" >&2
    echo "Set RDMA_CORE_ROOT to the custom rdma-core checkout." >&2
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is required to build rdma-core." >&2
    exit 1
fi

# Replacing a shared object underneath a running MPI process can leave that
# process with a mixture of old and new mappings.  Refuse the update instead
# of creating a hard-to-reproduce crash.
account_name=$(id -un)
if pgrep -u "$(id -u)" -f \
    '(^|/)(osu_(allreduce|alltoall)|mpiexec|hydra_pmi_proxy)( |$)' \
    >/dev/null 2>&1; then
    echo "An OSU/Hydra process owned by $account_name is still running." >&2
    echo "Stop the MPI job before replacing its rdma-core runtime libraries." >&2
    exit 1
fi

echo "Configuring rdma-core: $rdma_core_build"
cmake -S "$rdma_core_root" -B "$rdma_core_build" \
    -DIN_PLACE=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DNO_MAN_PAGES=1

echo "Building rdma-core with $jobs jobs"
cmake --build "$rdma_core_build" -j "$jobs"

if [[ "$rebuild_mvapich" == 1 ]]; then
    echo "Rebuilding ordinary and Hollow RC MVAPICH2 against the new headers"
    RDMA_CORE_ROOT="$rdma_core_root" \
    RDMA_CORE_BUILD="$rdma_core_build" \
    JOBS="$jobs" \
        "$script_dir/build_mvapich2.sh" ordinary
    RDMA_CORE_ROOT="$rdma_core_root" \
    RDMA_CORE_BUILD="$rdma_core_build" \
    JOBS="$jobs" \
        "$script_dir/build_mvapich2.sh" hollow
    exit 0
fi

lib_dir="$rdma_core_build/lib"
if [[ ! -r "$lib_dir/libibverbs.so" || ! -r "$lib_dir/libmlx5.so" ]]; then
    echo "rdma-core build did not produce libibverbs.so and libmlx5.so." >&2
    exit 1
fi

shopt -s nullglob
runtime_files=(
    "$lib_dir/"libibverbs.so*
    "$lib_dir/"libmlx5.so*
    "$lib_dir/"libmlx5-rdmav*.so
    "$lib_dir/"librdmacm.so*
)
if (( ${#runtime_files[@]} == 0 )); then
    echo "No rdma-core runtime libraries found in $lib_dir" >&2
    exit 1
fi

prefixes=(
    "$workspace_dir/mvapich2-2.3.7-install"
    "$workspace_dir/mvapich2-2.3.7-hollow-install"
)
deployed=0
for prefix in "${prefixes[@]}"; do
    if [[ ! -x "$prefix/bin/mpiexec" ]]; then
        echo "Skipping absent MVAPICH2 installation: $prefix"
        continue
    fi

    runtime_dir="$prefix/lib/hollow-rc-rdma"
    mkdir -p "$runtime_dir" "$prefix/etc"
    cp -a "${runtime_files[@]}" "$runtime_dir/"
    printf '%s\n' "$lib_dir" > "$prefix/etc/hollow-rc-rdma-libdir"

    if ! cmp -s "$(readlink -f "$lib_dir/libibverbs.so")" \
                  "$(readlink -f "$runtime_dir/libibverbs.so")" ||
       ! cmp -s "$(readlink -f "$lib_dir/libmlx5.so")" \
                  "$(readlink -f "$runtime_dir/libmlx5.so")"; then
        echo "Runtime verification failed for: $prefix" >&2
        exit 1
    fi

    echo "Updated rdma-core runtime: $runtime_dir"
    deployed=$((deployed + 1))
done

if (( deployed == 0 )); then
    echo "No existing MVAPICH2 installation was found." >&2
    echo "Run contrib/hollow-rc/bootstrap.sh for the first full installation." >&2
    exit 1
fi

echo
echo "rdma-core rebuild and runtime deployment completed."
echo "MVAPICH2 itself was not rebuilt (REBUILD_MVAPICH=0)."
echo "The existing run_osu_collective.sh commands are ready to use."
