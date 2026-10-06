#!/usr/bin/env bash
# Apply the two Banners-Turnip KGSL fixes used by the pinned native RT build.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "${1:?usage: apply_common.sh <mesa-dir>}"
for name in kgsl-syncobj-merge-ts-fd.patch kgsl-zero-timeout-poll.patch; do
    echo "[common] applying $name"
    patch -p1 -N --fuzz=3 --no-backup-if-mismatch < "$here/$name"
done
[[ "$(grep -c 'int ret_fd = kgsl_syncobj_ts_to_fd(&ret)' src/freedreno/vulkan/tu_knl_kgsl.cc)" == 2 ]] || {
    echo 'KGSL sync-object merge fix was not applied' >&2; exit 1;
}
grep -q 'kgsl_timestamp_retired(fd, context_id, timestamp) ? VK_SUCCESS : VK_TIMEOUT' \
    src/freedreno/vulkan/tu_knl_kgsl.cc || {
    echo 'KGSL zero-timeout poll fix was not applied' >&2; exit 1;
}
