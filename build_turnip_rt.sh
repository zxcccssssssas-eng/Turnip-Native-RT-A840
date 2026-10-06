#!/usr/bin/env bash
# Native Turnip RT pipelines. Kept separate from the ordinary Mesa-main builds.
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
work="${TURNIP_RT_WORKDIR:-$root/turnip_workdir/native-rt}"
source_dir="${TURNIP_RT_SOURCE:-$work/mesa}"
build_dir="${TURNIP_RT_BUILD:-$source_dir/build-android-aarch64}"
output_dir="${TURNIP_RT_OUTPUT:-$root/artifacts/native-rt}"
rt_commit="bec9c385f41483fcf0dcc04da7564db1750ed4c7"
rt_url="https://gitlab.freedesktop.org/cwabbott0/mesa.git"
ndk_dir="${ANDROID_NDK_HOME:-$root/turnip_workdir/android-ndk-r29}"
api=35

for dependency in git meson ninja python3 curl unzip patch glslangValidator flex bison; do
    command -v "$dependency" >/dev/null || { echo "Missing dependency: $dependency" >&2; exit 1; }
done
python3 -c 'import mako, yaml, packaging' || { echo 'Install Python mako, pyyaml and packaging.' >&2; exit 1; }
mkdir -p "$work" "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"

if [[ ! -x "$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${api}-clang" ]]; then
    [[ -z "${ANDROID_NDK_HOME:-}" ]] || { echo 'ANDROID_NDK_HOME must point to NDK r29.' >&2; exit 1; }
    archive="$root/turnip_workdir/android-ndk-r29-linux.zip"
    [[ -f "$archive" ]] || curl --fail --location --retry 3 \
        https://dl.google.com/android/repository/android-ndk-r29-linux.zip -o "$archive"
    unzip -q -o "$archive" -d "$root/turnip_workdir"
fi
ndk="$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/bin"

if [[ ! -d "$source_dir" ]]; then
    mkdir -p "$source_dir"
    git -C "$source_dir" init -q
    git -C "$source_dir" remote add origin "$rt_url"
    git -C "$source_dir" fetch --depth=1 origin "$rt_commit"
    git -C "$source_dir" checkout -q --detach FETCH_HEAD
fi
[[ "$(git -C "$source_dir" rev-parse HEAD)" == "$rt_commit" ]] || {
    echo "Native RT requires pinned Mesa $rt_commit; use a separate source directory." >&2; exit 1;
}

# Apply once, without resetting or discarding local source changes.
prepared="$source_dir/.banners-native-rt-prepared"
if [[ ! -f "$prepared" ]]; then
    # Refuse an arbitrary modified checkout; a clean, pinned tree is reviewable.
    [[ -z "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]] || {
        echo 'Source has local changes. Use a fresh TURNIP_RT_SOURCE directory.' >&2; exit 1;
    }
    bash "$root/patches/common/apply_common.sh" "$source_dir"
    patch --directory="$source_dir" -p1 --fuzz=0 < "$root/patches/raytracing/kgsl-aqe-enable.patch"
    (cd "$source_dir" && python3 "$root/patches/a840v2.py")
    git -C "$source_dir" diff --binary | sha256sum | awk '{print $1}' > "$prepared"
fi
expected_diff="$(cat "$prepared")"
actual_diff="$(git -C "$source_dir" diff --binary | sha256sum | awk '{print $1}')"
[[ "$actual_diff" == "$expected_diff" ]] || {
    echo 'Prepared RT source changed. Inspect the changes and use a fresh source directory.' >&2; exit 1;
}
grep -q 'device->has_aqe = tu_kgsl_get_aqe(fd)' "$source_dir/src/freedreno/vulkan/tu_knl_kgsl.cc"
grep -q 'KHR_ray_tracing_pipeline = has_rt_pipeline && has_raytracing' "$source_dir/src/freedreno/vulkan/tu_device.cc"

cross="$work/android-aarch64.txt"
export BANNER_RT_NDK="$ndk" BANNER_RT_CROSS="$cross"
python3 - <<'PY'
import os
from pathlib import Path
ndk = Path(os.environ['BANNER_RT_NDK']).resolve()
# Meson strings use backslash escapes; reject ambiguous build paths.
if any(c in str(ndk) for c in "'\\\n"):
    raise SystemExit('NDK path contains unsupported characters')
Path(os.environ['BANNER_RT_CROSS']).write_text(f'''[binaries]
c = '{ndk}/aarch64-linux-android35-clang'
cpp = ['{ndk}/aarch64-linux-android35-clang++', '-static-libstdc++']
ar = '{ndk}/llvm-ar'
strip = '{ndk}/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=', '/usr/bin/pkg-config']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
''')
PY
setup=()
[[ ! -f "$build_dir/meson-private/coredata.dat" ]] || setup+=(--reconfigure)
meson setup "${setup[@]}" "$build_dir" "$source_dir" --cross-file "$cross" \
    -Dbuildtype=release -Dstrip=false -Dplatforms=android -Dvideo-codecs= \
    -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
    -Dandroid-stub=true -Dplatform-sdk-version=36 -Dandroid-libbacktrace=disabled \
    -Degl=disabled -Dllvm=disabled -Dtools= -Dexpat=disabled \
    "-Dcpp_args=['-Wno-c++11-narrowing', '-Wno-missing-field-initializers']" \
    "-Dc_args=['-Wno-error']"
ninja -C "$build_dir" -j "${TURNIP_RT_JOBS:-8}" src/freedreno/vulkan/libvulkan_freedreno.so
stage="$output_dir/package"
mkdir -p "$stage"
cp "$build_dir/src/freedreno/vulkan/libvulkan_freedreno.so" "$stage/libvulkan_freedreno.so"
"$ndk/llvm-strip" --strip-unneeded "$stage/libvulkan_freedreno.so"
export BANNER_RT_STAGE="$stage" BANNER_RT_SOURCE="$source_dir" BANNER_RT_COMMIT="$rt_commit" BANNER_RT_DIFF="$actual_diff"
python3 - <<'PY'
import hashlib, json, os
from pathlib import Path
stage = Path(os.environ['BANNER_RT_STAGE'])
meta = dict(schemaVersion=1, name='Turnip Native RT A840 — experimental',
    description='Native Mesa ray tracing pipelines and ray queries. Requires enabled KGSL RT/AQE. Experimental pinned Connor Abbott RT v3 stack; tested on Lenovo TB323FU Adreno 840v2. No Qualcomm binaries.',
    author='The412Banner / Connor Abbott (Mesa)', packageVersion='1', vendor='Mesa',
    driverVersion='Mesa 26.2.0-devel / Vulkan 1.4.354', minApi=35,
    libraryName='libvulkan_freedreno.so')
(stage/'meta.json').write_text(json.dumps(meta, indent=2)+'\n')
report = dict(mesaSource='https://gitlab.freedesktop.org/cwabbott0/mesa',
    mesaCommit=os.environ['BANNER_RT_COMMIT'], localDiffSha256=os.environ['BANNER_RT_DIFF'],
    driverSha256=hashlib.sha256((stage/'libvulkan_freedreno.so').read_bytes()).hexdigest(),
    minApi=35, nativeRayTracing=True, proprietaryLibrariesIncluded=False,
    hardwareValidation='See docs/NATIVE_RAY_TRACING.md. Building does not establish device compatibility.')
(stage.parent/'build-report.json').write_text(json.dumps(report, indent=2)+'\n')
PY
archive="$output_dir/Turnip-Native-RT-A840-experimental.zip"
# Rewrite the generated archive so stale members cannot survive a rebuild.
python3 - "$stage" "$archive" <<'PY'
import sys, zipfile
from pathlib import Path
with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in ('libvulkan_freedreno.so', 'meta.json'):
        z.write(Path(sys.argv[1])/name, name)
PY
echo "Native RT package: $archive"
echo 'Run tests/raytracing/run_tablet.sh on the target device before using this experimental driver.'
