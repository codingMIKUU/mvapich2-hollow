#!/usr/bin/env bash
set -euo pipefail

ssh_bin=${MV2_SSH_BIN:-/usr/bin/ssh}
remote_workspace=${MV2_REMOTE_WORKSPACE:-zxm}
remote_install=${MV2_REMOTE_INSTALL:-}

if [[ -z "$remote_install" ]]; then
    echo "MV2_REMOTE_INSTALL is not set for the Hydra SSH launcher." >&2
    exit 2
fi

# Hydra invokes its launcher as:
#   launcher -x [user@]host /local/absolute/path/hydra_pmi_proxy ...
# The two hosts may use different account names, so the launcher's absolute
# prefix cannot be reused remotely.  Preserve Hydra's SSH option, resolve the
# remote account home once, and launch the matching installed proxy there.
ssh_options=()
while [[ ${1:-} == -* ]]; do
    case "$1" in
        -x|-q|-T|-4|-6)
            ssh_options+=("$1")
            shift
            ;;
        *)
            echo "Unsupported Hydra SSH option: $1" >&2
            exit 2
            ;;
    esac
done

target=${1:-}
local_proxy=${2:-}
if [[ -z "$target" || -z "$local_proxy" ]]; then
    echo "Invalid Hydra SSH launcher arguments." >&2
    exit 2
fi
shift 2

# Hydra 3.2.1 may retain the display quotes around an absolute proxy path
# when it invokes a custom launcher.
local_proxy=${local_proxy#\"}
local_proxy=${local_proxy%\"}
local_proxy=${local_proxy#\'}
local_proxy=${local_proxy%\'}

if [[ ${local_proxy##*/} != hydra_pmi_proxy ]]; then
    exec "$ssh_bin" "${ssh_options[@]}" "$target" "$local_proxy" "$@"
fi

probe='account_home=$(getent passwd "$(id -u)" | cut -d: -f6); for candidate in "$account_home/'"$remote_workspace/$remote_install"'/bin/hydra_pmi_proxy" "$account_home/'"$remote_install"'/bin/hydra_pmi_proxy"; do if [[ -x "$candidate" ]]; then printf "%s\n" "$candidate"; exit 0; fi; done; exit 1'
if ! remote_proxy=$("$ssh_bin" "${ssh_options[@]}" "$target" "$probe"); then
    echo "Cannot find $remote_install/bin/hydra_pmi_proxy on $target." >&2
    exit 1
fi

exec "$ssh_bin" "${ssh_options[@]}" "$target" "$remote_proxy" "$@"
