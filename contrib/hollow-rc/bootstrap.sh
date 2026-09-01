#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd -- "$script_dir/../.." && pwd)
workspace_dir=$(dirname -- "$source_dir")

jobs=${JOBS:-8}
install_deps=${INSTALL_DEPS:-1}
rdma_core_root=${RDMA_CORE_ROOT:-"$workspace_dir/rdma-core"}
rdma_core_build=${RDMA_CORE_BUILD:-"$rdma_core_root/build-codex"}
rdma_core_repo=${RDMA_CORE_REPO:-https://github.com/codingMIKUU/rdma-core.git}
rdma_core_branch=${RDMA_CORE_BRANCH:-srm-cq-progress-poll}

echo "[1/5] Checking build dependencies"
if [[ "$install_deps" == 1 ]] && command -v apt-get >/dev/null 2>&1; then
    packages=(
        build-essential cmake git pkg-config autoconf automake libtool
        bison flex gfortran libnl-3-dev libnl-route-3-dev libnuma-dev
        numactl libudev-dev libibmad-dev libibumad-dev
    )
    missing=()
    for package in "${packages[@]}"; do
        if ! dpkg-query -W -f='${Status}' "$package" 2>/dev/null |
             grep -q 'install ok installed'; then
            missing+=("$package")
        fi
    done
    if (( ${#missing[@]} )); then
        sudo apt-get update
        sudo apt-get install -y "${missing[@]}"
    fi
fi

echo "[2/5] Preparing custom rdma-core: $rdma_core_root"
if [[ ! -d "$rdma_core_root/.git" ]]; then
    git clone --branch "$rdma_core_branch" "$rdma_core_repo" "$rdma_core_root"
fi

echo "[3/5] Building custom rdma-core: $rdma_core_build"
cmake -S "$rdma_core_root" -B "$rdma_core_build" \
    -DIN_PLACE=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DNO_MAN_PAGES=1
cmake --build "$rdma_core_build" -j "$jobs"

echo "[4/5] Building and installing ordinary MVAPICH2"
RDMA_CORE_ROOT="$rdma_core_root" \
RDMA_CORE_BUILD="$rdma_core_build" \
JOBS="$jobs" \
"$script_dir/build_mvapich2.sh" ordinary

echo "[5/5] Building and installing Hollow RC MVAPICH2"
RDMA_CORE_ROOT="$rdma_core_root" \
RDMA_CORE_BUILD="$rdma_core_build" \
JOBS="$jobs" \
"$script_dir/build_mvapich2.sh" hollow

ordinary_prefix="$workspace_dir/mvapich2-2.3.7-install"
hollow_prefix="$workspace_dir/mvapich2-2.3.7-hollow-install"
for prefix in "$ordinary_prefix" "$hollow_prefix"; do
    for required in \
        "$prefix/bin/mpicc" \
        "$prefix/bin/mpiexec" \
        "$prefix/libexec/osu-micro-benchmarks/mpi/collective/osu_allreduce" \
        "$prefix/libexec/osu-micro-benchmarks/mpi/collective/osu_alltoall" \
        "$prefix/lib/hollow-rc-rdma/libibverbs.so" \
        "$prefix/lib/hollow-rc-rdma/libmlx5.so"; do
        if [[ ! -e "$required" ]]; then
            echo "Installation verification failed; missing: $required" >&2
            exit 1
        fi
    done

    if ! cmp -s "$(readlink -f "$rdma_core_build/lib/libibverbs.so")" \
                  "$(readlink -f "$prefix/lib/hollow-rc-rdma/libibverbs.so")" ||
       ! cmp -s "$(readlink -f "$rdma_core_build/lib/libmlx5.so")" \
                  "$(readlink -f "$prefix/lib/hollow-rc-rdma/libmlx5.so")"; then
        echo "Bundled rdma-core verification failed for: $prefix" >&2
        exit 1
    fi
    echo "Verified installation: $prefix"
done

echo
echo "Hollow RC userspace installation completed."
echo "ordinary: $ordinary_prefix"
echo "hollow:   $hollow_prefix"
echo "Run OSU tests from this source tree with:"
echo "  contrib/hollow-rc/run_osu_collective.sh ordinary allreduce [OSU arguments]"
echo "  contrib/hollow-rc/run_osu_collective.sh xrc allreduce [OSU arguments]"
echo "  contrib/hollow-rc/run_osu_collective.sh hollow allreduce [OSU arguments]"
