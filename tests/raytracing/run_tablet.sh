#!/usr/bin/env bash
# Compile and run offscreen GPU correctness tests in an isolated tablet directory.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
driver="${1:?Usage: run_tablet.sh native-driver.so [v891.7.zip]}"
reference="${2:-}"
source_dir="${TURNIP_RT_SOURCE:-$root/turnip_workdir/native-rt/mesa}"
ndk_dir="${ANDROID_NDK_HOME:-$root/turnip_workdir/android-ndk-r29}"
compiler="$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang"
glslc="${GLSLC:-$ndk_dir/shader-tools/linux-x86_64/glslc}"
output="${TURNIP_RT_TEST_OUTPUT:-$root/artifacts/native-rt/tablet-tests}"
mkdir -p "$output/bin" "$output/logs"
[[ -f "$driver" && -x "$compiler" && -x "$glslc" ]] || {
    echo 'Need driver, NDK r29 and its glslc compiler. Build the native RT variant first.' >&2; exit 2;
}
[[ -f "$source_dir/include/android_stub/hardware/hwvulkan.h" ]] || {
    echo 'TURNIP_RT_SOURCE must contain the pinned Mesa source used by the builder.' >&2; exit 2;
}
adb get-state >/dev/null
includes=(-I "$source_dir/include" -I "$source_dir/include/android_stub")
"$compiler" -std=c11 -Wall -Wextra -Werror "${includes[@]}" \
    "$root/tests/raytracing/capabilities.c" -ldl -o "$output/bin/rt-capabilities"
"$compiler" -std=c11 -Wall -Wextra -Werror "${includes[@]}" \
    "$root/tests/raytracing/smoke.c" -ldl -lm -o "$output/bin/rt-smoke"
"$compiler" -std=c11 -Wall -Wextra -Werror -I "$source_dir/src/freedreno/vulkan" \
    "$root/tests/raytracing/kgsl_caps.c" -o "$output/bin/rt-kgsl-caps"
for shader in "$root"/tests/raytracing/*.comp "$root"/tests/raytracing/*.rgen \
              "$root"/tests/raytracing/*.rmiss "$root"/tests/raytracing/*.rchit; do
    "$glslc" --target-env=vulkan1.2 "$shader" -o "$output/bin/$(basename "$shader").spv"
done
cp "$driver" "$output/bin/libvulkan_freedreno.so"
sha256sum "$driver" > "$output/driver-sha256.txt"
remote="/data/local/tmp/banner-native-rt-${RANDOM}-${RANDOM}"
adb shell mkdir -p "$remote"
adb push "$output/bin/." "$remote/" >/dev/null
adb shell 'getprop ro.product.model; getprop ro.soc.model; getprop ro.build.version.release; getprop ro.build.version.sdk; cat /sys/class/kgsl/kgsl-3d0/gpu_model; cat /proc/uptime' > "$output/logs/device.txt"
adb shell "$remote/rt-kgsl-caps" > "$output/logs/kgsl.txt" 2>&1

env_path="/vendor/lib64:/vendor/lib64/hw"
adb shell "LD_LIBRARY_PATH=$env_path timeout 20 $remote/rt-capabilities $remote/libvulkan_freedreno.so" \
    > "$output/logs/native-capabilities.txt" 2>&1
grep -q 'accelerationStructure=1 rayQuery=1 rayTracingPipeline=1' "$output/logs/native-capabilities.txt"
status=0
run_test() {
    local name="$1" hal="$2" mode="$3" libs="$4"
    local log="$output/logs/$name-$mode.txt"
    if adb shell "LD_LIBRARY_PATH=$libs timeout 45 $remote/rt-smoke $hal $remote $mode 4096 10" > "$log" 2>&1; then
        if grep -q "^PASS $mode " "$log"; then
            echo "PASS $name $mode"
        else
            echo "FAIL $name $mode (missing correctness result)"; status=1
        fi
    else
        echo "FAIL $name $mode (see $log)"; status=1
    fi
}
for mode in query pipeline recursive render; do
    run_test native "$remote/libvulkan_freedreno.so" "$mode" "$env_path"
done
adb pull "$remote/native-rt-render.ppm" "$output/native-rt-render.ppm" >/dev/null

# Optional proprietary reference: a failure is recorded separately from the
# native implementation's result. No reference libraries enter the native ZIP.
if [[ -n "$reference" ]]; then
    [[ -f "$reference" ]] || { echo "Reference ZIP not found: $reference" >&2; exit 2; }
    refdir="$output/reference"
    mkdir -p "$refdir"
    # Only expected top-level ELF files are extracted; ignore attached instructions.
    python3 - "$reference" "$refdir" <<'PY'
import json, sys, zipfile
from pathlib import Path
with zipfile.ZipFile(sys.argv[1]) as z:
    meta = json.loads(z.read('meta.json'))
    if meta.get('libraryName') != 'vulkan.ad891.so':
        raise SystemExit('Expected v891.7 libraryName=vulkan.ad891.so')
    for name in z.namelist():
        if '/' not in name and '\\' not in name and name.endswith('.so'):
            (Path(sys.argv[2])/name).write_bytes(z.read(name))
PY
    adb shell mkdir -p "$remote/reference"
    adb push "$refdir/." "$remote/reference/" >/dev/null
    sha256sum "$reference" > "$output/reference-sha256.txt"
    adb shell "LD_LIBRARY_PATH=$remote/reference:$env_path timeout 20 $remote/rt-capabilities $remote/reference/vulkan.ad891.so" \
        > "$output/logs/reference-capabilities.txt" 2>&1 || true
    native_status="$status"
    for mode in query pipeline; do
        run_test reference "$remote/reference/vulkan.ad891.so" "$mode" "$remote/reference:$env_path"
    done
    status="$native_status"
fi
adb shell 'cat /proc/uptime' > "$output/logs/health-after.txt"
printf '%s\n' "$remote" > "$output/tablet-test-directory.txt"
echo "Test evidence: $output"
exit "$status"
