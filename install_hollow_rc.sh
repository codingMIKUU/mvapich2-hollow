#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

exec "$source_dir/contrib/hollow-rc/bootstrap.sh"
