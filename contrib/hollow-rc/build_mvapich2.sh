#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 ordinary|hollow" >&2
    exit 2
}

mode=${1:-}
case "$mode" in
    ordinary|hollow) ;;
    *) usage ;;
esac

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd -- "$script_dir/../.." && pwd)
workspace_dir=$(dirname -- "$source_dir")
rdma_core_dir=${RDMA_CORE_ROOT:-"$workspace_dir/rdma-core"}
rdma_build_dir=${RDMA_CORE_BUILD:-"$rdma_core_dir/build-codex"}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

if [[ "$mode" == hollow ]]; then
    build_dir=${MVAPICH2_BUILD_DIR:-"$workspace_dir/mvapich2-build-hollow"}
    prefix=${MVAPICH2_PREFIX:-"$workspace_dir/mvapich2-2.3.7-hollow-install"}
    mode_option=--enable-hollow-rc
else
    build_dir=${MVAPICH2_BUILD_DIR:-"$workspace_dir/mvapich2-build-ordinary"}
    prefix=${MVAPICH2_PREFIX:-"$workspace_dir/mvapich2-2.3.7-install"}
    mode_option=--disable-hollow-rc
fi

verbs_header="$rdma_build_dir/include/infiniband/verbs.h"
verbs_library="$rdma_build_dir/lib/libibverbs.so"
if [[ ! -r "$verbs_header" || ! -r "$verbs_library" ]]; then
    echo "Custom rdma-core build is incomplete: $rdma_build_dir" >&2
    echo "Build rdma-core first, or set RDMA_CORE_BUILD." >&2
    exit 1
fi

mkdir -p "$build_dir"
cd "$build_dir"

CPPFLAGS="-I$rdma_build_dir/include ${CPPFLAGS:-}" \
LDFLAGS="-L$rdma_build_dir/lib ${LDFLAGS:-}" \
CC=${CC:-gcc} CXX=${CXX:-g++} \
"$source_dir/configure" \
    --prefix="$prefix" \
    "$mode_option" \
    --disable-fortran \
    --with-hwloc=v2 \
    --with-ib-include="$rdma_build_dir/include" \
    --with-ib-libpath="$rdma_build_dir/lib"

make -j"$jobs"
make -j"$jobs" install
install -m 0755 "$script_dir/mv2_hca_rank_wrapper.sh" "$prefix/bin/"
install -m 0755 "$script_dir/mv2_hydra_ssh_wrapper.sh" "$prefix/bin/"

# Keep the custom userspace RDMA runtime with each MVAPICH installation.
# Hydra may propagate the launcher's HOME to a remote rank, so runtime lookup
# must not depend on HOME or on the source checkout having a particular name.
runtime_dir="$prefix/lib/hollow-rc-rdma"
mkdir -p "$runtime_dir" "$prefix/etc"
shopt -s nullglob
runtime_files=(
    "$rdma_build_dir/lib/"libibverbs.so*
    "$rdma_build_dir/lib/"libmlx5.so*
    "$rdma_build_dir/lib/"libmlx5-rdmav*.so
    "$rdma_build_dir/lib/"librdmacm.so*
)
if (( ${#runtime_files[@]} == 0 )); then
    echo "No rdma-core runtime libraries found in $rdma_build_dir/lib" >&2
    exit 1
fi
cp -a "${runtime_files[@]}" "$runtime_dir/"
printf '%s\n' "$rdma_build_dir/lib" > "$prefix/etc/hollow-rc-rdma-libdir"

echo "Installed MVAPICH2 ($mode) in: $prefix"
echo "Custom rdma-core runtime in:   $rdma_build_dir/lib"
echo "Bundled RDMA runtime in:       $runtime_dir"
