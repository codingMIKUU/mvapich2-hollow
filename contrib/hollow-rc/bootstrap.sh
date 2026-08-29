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

if [[ "$install_deps" == 1 ]] && command -v apt-get >/dev/null 2>&1; then
    packages=(
        build-essential cmake git pkg-config autoconf automake libtool
        bison flex gfortran libnl-3-dev libnl-route-3-dev libnuma-dev
        libudev-dev libibmad-dev libibumad-dev
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

if [[ ! -d "$rdma_core_root/.git" ]]; then
    git clone --branch "$rdma_core_branch" "$rdma_core_repo" "$rdma_core_root"
fi

cmake -S "$rdma_core_root" -B "$rdma_core_build" \
    -DIN_PLACE=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DNO_MAN_PAGES=1
cmake --build "$rdma_core_build" -j "$jobs"

RDMA_CORE_ROOT="$rdma_core_root" \
RDMA_CORE_BUILD="$rdma_core_build" \
JOBS="$jobs" \
"$script_dir/build_mvapich2.sh" ordinary

RDMA_CORE_ROOT="$rdma_core_root" \
RDMA_CORE_BUILD="$rdma_core_build" \
JOBS="$jobs" \
"$script_dir/build_mvapich2.sh" hollow

echo
echo "Hollow RC userspace installation completed."
echo "ordinary: $workspace_dir/mvapich2-2.3.7-install"
echo "hollow:   $workspace_dir/mvapich2-2.3.7-hollow-install"
