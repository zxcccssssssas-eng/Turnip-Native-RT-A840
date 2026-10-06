# Turnip Native RT for Adreno 840

Experimental **native Mesa Turnip ray tracing** for Android, tested on a Lenovo
TB323FU tablet with Adreno 840v2. Supports `VK_KHR_ray_tracing_pipeline`,
`VK_KHR_ray_query` and `VK_KHR_acceleration_structure` when KGSL reports both
ray tracing and AQE enabled.

[Download the tested driver](https://github.com/zxcccssssssas-eng/Turnip-Native-RT-A840/releases/tag/v0.1.0-experimental) ·
[All releases](https://github.com/zxcccssssssas-eng/Turnip-Native-RT-A840/releases) ·
[Tablet test report](docs/NATIVE_RAY_TRACING.md) ·
[Raw test evidence](docs/native-rt-tablet-evidence.json)

The implementation comes from Connor Abbott's pinned experimental RT v3 Mesa
stack. This project adds the missing KGSL AQE assignment and packages it with
the Banners-Turnip synchronization fixes and Adreno 840v2 device identification.
The supplied Qualcomm v891.7 archive was used as a reference during testing.
No proprietary driver libraries are included in the native driver or this repository.

## Tablet results

Tested October 7, 2026 on Lenovo TB323FU / SM8850P / Adreno 840v2 / Android 16.

| Test | Result |
| --- | --- |
| Ray queries: 4,096 rays × 10 dispatches | Passed |
| RT pipelines: 4,096 rays × 10 dispatches | Passed |
| Recursive ray tracing, depth 2 | Passed |
| 128 × 96 rendered image, every pixel checked over 10 dispatches | Passed |

Actual GPU-rendered output from the tablet:

![Native Turnip ray-traced triangle](docs/images/native-rt-render.png)

The build remains experimental. These tests do not establish game compatibility
or full Vulkan conformance. Validation covers one Adreno 840v2 tablet.

## Install

1. Download `Turnip-Native-RT-A840-experimental.zip` from **Releases**.
2. Import it in an AdrenoTools-compatible app's GPU driver settings.
3. Select **Turnip Native RT A840 — experimental** for that app.

Requires Android API 35 or newer and enabled KGSL RT/AQE capabilities. The
package is for Android/bionic; it is not a Linux/glibc or Wayland driver.
System driver replacement is not required.

## Build

Install Git, Meson, Ninja, curl, unzip, patch, glslangValidator, flex, bison and
Python mako, pyyaml and packaging on Linux. NDK r29 is downloaded automatically
or can be supplied through `ANDROID_NDK_HOME`.

```sh
bash build_turnip_rt.sh
```

The builder pins Mesa at `bec9c385f41483fcf0dcc04da7564db1750ed4c7` and refuses
to discard local source edits. Output:

```text
artifacts/native-rt/Turnip-Native-RT-A840-experimental.zip
artifacts/native-rt/build-report.json
```

Alternatively, run **Build Turnip Native RT (Experimental)** from the **Actions**
tab. It uploads build artifacts without automatically publishing a release.

## Reproduce physical-device tests

With an ADB-connected tablet:

```sh
bash tests/raytracing/run_tablet.sh \
  artifacts/native-rt/package/libvulkan_freedreno.so
```

The optional second argument is the path to the v891.7 reference ZIP. Use
`ANDROID_SERIAL` when multiple devices are connected. These direct HAL tests
need access to `/dev/kgsl-3d0` and vendor libraries; the recorded run used the
tablet's existing rooted ADB connection. Tests run in isolated temporary
directories without replacing system drivers or changing app settings.

## Credits and license

- [Connor Abbott / Mesa](https://gitlab.freedesktop.org/cwabbott0/mesa/-/tree/tu-rt-pipelines-v3): native RT implementation, with upstream MIT copyright headers preserved.
- [The412Banner / Banners-Turnip](https://github.com/The412Banner/Banners-Turnip): packaging foundation, common KGSL fixes and A840v2 identification.

Repository build scripts and downstream changes are distributed under
[GPL-3.0](LICENSE). Upstream files retain their original licenses. See
[NOTICE](NOTICE), [RT provenance](patches/raytracing/SOURCE), and
[KGSL patch provenance](patches/common/SOURCE).
