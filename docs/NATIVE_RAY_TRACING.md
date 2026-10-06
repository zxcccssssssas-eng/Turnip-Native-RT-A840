# Native Turnip ray tracing on Adreno 840

This experimental Android driver implements ray tracing **inside Mesa Turnip**.
It builds shader continuations, native IR3 shaders, shader binding tables and
AQE command streams. It does not forward Vulkan calls to a Qualcomm driver.

The implementation is Connor Abbott's [RT v3 source](https://gitlab.freedesktop.org/cwabbott0/mesa/-/tree/tu-rt-pipelines-v3),
pinned at `bec9c385f41483fcf0dcc04da7564db1750ed4c7` (July 16, 2026).
The original 33 commits are preserved in `patches/raytracing/turnip-rt-v3.patch`.
The variant uses Mesa 26.2.0-devel / Vulkan 1.4.354 rather than the moving
Mesa-main source used by ordinary Banners-Turnip builds. Upstream copyright and
MIT headers are preserved; full provenance is in `patches/raytracing/SOURCE`.

## Local fixes and capability checks

The RT series defines `tu_kgsl_get_aqe()` but omits the assignment to
`device->has_aqe` in KGSL device initialization. `kgsl-aqe-enable.patch` adds
that assignment. It reads `KGSL_PROP_IS_AQE_ENABLED` and respects the result.
The existing ray-tracing SW-fuse check and nonzero `max_rt_threads` requirement
also remain in effect. The Lenovo tablet reports both RT and AQE enabled.

The build also applies Banners-Turnip's common KGSL synchronization fixes and
its A840v2 chip-ID correction (`0x44050A21`). It leaves the ordinary build
recipes unchanged. The A8xx mesh/wave32 series is not part of this RT variant.

## Build and reproduce tablet tests

On Linux, install Git, Meson, Ninja, curl, unzip, zip, patch, glslangValidator,
flex, bison, and Python mako, pyyaml and packaging. The script reuses NDK r29 in
`turnip_workdir/android-ndk-r29`, or downloads it from Google. An explicit
`ANDROID_NDK_HOME` can point to NDK r29. It pins the Mesa commit and refuses to
discard local source changes.

```sh
bash build_turnip_rt.sh
bash tests/raytracing/run_tablet.sh \
  artifacts/native-rt/package/libvulkan_freedreno.so \
  /path/to/v891.7.zip
```

The second argument is optional. Use `ANDROID_SERIAL` when more than one
Android device is connected. Tests require ADB access to `/dev/kgsl-3d0` and
the tablet's vendor libraries; these tests were run with the existing rooted
ADB connection. No system driver, kernel, app setting, or firmware is replaced.
Each run loads the driver from a separate directory under `/data/local/tmp`.
The runner preserves logs, GPU-rendered pixels and the tested driver hash.

The generated AdrenoTools package is
`artifacts/native-rt/Turnip-Native-RT-A840-experimental.zip`. It contains only
`libvulkan_freedreno.so` and `meta.json`, declares `minApi: 35`, and is for
Android/bionic. It is not a Linux/glibc or Bannerlator Wayland driver.
The dedicated manual Actions workflow builds and uploads the package without
publishing a release. CI package checks are distinct from physical-device tests.

## Physical-device validation — October 7, 2026

Tablet: Lenovo TB323FU; SoC: SM8850P; Android 16 (API 36);
GPU: `Adreno840v2`; KGSL RT property `0x2d = 1`, AQE property `0x30 = 1`.

| Check | Result |
| --- | --- |
| Native acceleration structures, ray query and RT pipeline feature queries | All enabled |
| Triangle BLAS and TLAS construction | Passed |
| Native ray queries: 4,096 rays × 10 dispatches | All hit/miss values correct |
| Native ray-tracing pipeline: 4,096 rays × 10 dispatches | All hit/miss payloads correct |
| Recursive pipeline: closest-hit shader launches a secondary miss ray, 4,096 primary rays × 10 dispatches | All payloads correct at recursion depth 2 |
| Native triangle render: 128 × 96 pixels × 10 dispatches | Every RGBA pixel checked against CPU geometry/barycentrics, tolerance ≤1 channel unit |
| Stock Qualcomm driver control | Hit/miss tests passed |
| Reboot during native tests | None observed; uptime stayed continuous |

`smoke.c` enables only required device features, builds BLAS/TLAS on the GPU,
reads results from coherent host memory after a fence and a shader-to-host
barrier, and reports failure on any incorrect ray or pixel. Vulkan resources
are destroyed after the GPU completes. Direct HAL tests use `_Exit` after
cleanup because vendor profiling threads otherwise race DSO static destructors.
The final packaged binary is tested again after stripping.
Raw final test results and the package/driver SHA256 values are recorded in
[`native-rt-tablet-evidence.json`](native-rt-tablet-evidence.json).

### v891.7 reference findings

The supplied ZIP contains proprietary Qualcomm binaries, not Mesa source. On
this tablet its actual feature query reports Vulkan **1.3.351**, despite its
metadata claiming 1.4.359; RT pipeline and ray-query features are advertised.
The ZIP omits `libllvm-qgl.so`, which its shader frontend loads dynamically.
With the tablet's existing, different-version `libllvm-qgl.so`, its ray-query
shader compilation crashes and RT pipeline compilation returns
`VK_ERROR_UNKNOWN`. Mixing the tablet's compiler frontend with v891.7 is also
blocked by missing `QCCGetLLVMVersion` / `gsl_get_device_drm_properties` symbols.
These failures are recorded separately from native Turnip results.

The archive is a capability/compiler compatibility reference. No binary code
was copied from it into Mesa and no Qualcomm libraries are included in the
native package. The tablet's stock driver served as the working rendering
control. The previously installed Turnip failed the minimal hit test, while
both the freshly built Mesa-main control and native RT build passed.

## Limits

This is a tested experimental build, not Vulkan conformance certification or
a game compatibility claim. Validation covers one Adreno 840v2 tablet, triangle
geometry, an identity instance transform, primary rays, misses, recursion depth 2,
repeated dispatches and a rendered image. Procedural intersections, any-hit,
callable shaders, indirect tracing, other GPU models and real games need
additional validation before treating this variant as a general replacement.
The pinned series includes upstream WIP diagnostics; the ordinary automated
release stream does not ship this experimental driver.
