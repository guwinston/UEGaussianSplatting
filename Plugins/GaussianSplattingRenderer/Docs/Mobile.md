# Mobile Platform Support

[English](Mobile.md) | [简体中文](Mobile.zh-CN.md)

This document covers mobile support, Android packaging, on-device profiling, tuning controls, and current limitations of `GaussianSplattingRenderer`. The validated mobile path is Vulkan SM5 on Android arm64.

## Support Matrix

| Platform | Backend | Status | Notes |
| --- | --- | --- | --- |
| Android arm64 | Vulkan SM5 (`SF_VULKAN_SM5_ANDROID`) | Validated | UE 5.5 cook, APK packaging, and an Adreno 650 device have been tested |
| Android arm64 | Vulkan ES3.1 | Unsupported | Mobile Vulkan disables the vertex-stage buffer SRVs used by the current renderer |
| Android arm64 | OpenGL ES3.1 | Unsupported | Not enabled by this project |
| iOS | Metal MRT / SM5 | Experimental | Device validation and performance tuning are incomplete |

The Android path retains full per-splat GPU sorting and transparent blending. It has no LOD, tiled sorting, or compute tile rasterizer yet, so large scenes are much more expensive on mobile GPUs than on desktop hardware.

## Rendering Pipeline

When sorting must be updated, the renderer performs:

1. Object-bounds frustum culling.
2. Per-splat culling and 32-bit depth-key generation over the merged stream.
3. Global GPU radix sorting through UE `SortGPUBuffers`.
4. Indirect draw-argument generation from the visible count.
5. Sorted ellipse rasterization and transparent blending.
6. Accumulation-texture composition in Integrate With UE mode, or direct SceneColor output in Direct mode.

A stationary camera reuses the previous sort. Camera or projection changes request a new sort. The radix-sort input remains the total splat count even when most splats are culled.

## Android Configuration

Key settings in `Config/DefaultEngine.ini`:

```ini
[/Script/AndroidRuntimeSettings.AndroidRuntimeSettings]
bBuildForArm64=True
bBuildForES31=False
bSupportsVulkan=False
bSupportsVulkanSM5=True
bPackageDataInsideApk=True
```

`bPackageDataInsideApk=True` embeds cooked shaders, maps, and content. Installing only the APK from an older APK-plus-OBB build can report missing global shaders or cooked content.

UE 5.5 does not select Vulkan SM5 for Adreno 6xx by default. The validated profile is enabled in `Config/DefaultDeviceProfiles.ini`:

```ini
[Android_Adreno6xx_Vulkan DeviceProfile]
+CVars=r.Android.DisableVulkanSM5Support=0
+CVars=r.Android.SupportsTimestampQueries=1
+CVars=r.GPUStatsEnabled=1
```

Verify required Vulkan features before forcing SM5 on another device. The validated device is an Adreno 650 using Vulkan 1.1.128.

## Toolchain and Packaging

UE 5.5 uses Android API 34, Build Tools 34.0.0, CMake 3.22.1, and NDK r25b (`25.1.8937393`). See [Android Packaging and Installation](../../../Scripts/README.Android.zh-CN.md) for setup details.

```powershell
# Development APK
.\Scripts\Build-Android.bat -Configuration Development

# Build, install, and launch
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch
```

Output:

```text
Packaged/Android-Development/UEGaussianSplatting-arm64.apk
```

Accept the USB debugging prompt when adb reports `unauthorized`. Keep the phone unlocked and approve installation if adb reports `INSTALL_FAILED_ABORTED: User rejected permissions`.

## Mobile Defaults

The Android Device Profile disables expensive or unvalidated features:

```ini
r.RayTracing=0
r.Lumen.DiffuseIndirect.Allow=0
r.Lumen.Reflections.Allow=0
r.Shadow.Virtual.Enable=0
r.GenerateMeshDistanceFields=0
r.GaussianSplat.EnableHigherOrderSH=0
r.GaussianSplat.RasterMode=0
r.GaussianSplat.CullMode=2
r.GaussianSplat.EnableAntialiasing=0
r.GaussianSplat.OpacityAwareBounds=1
```

These settings reduce shader and pixel costs but do not reduce the global-sort input count.

## Performance Profiling

Use a Development build:

```text
stat fps
stat unit
stat gpu
```

`stat gpu` reports GPU time in milliseconds, not GPU memory. Main plugin stages are:

| GPU stat | Meaning |
| --- | --- |
| Gaussian Splat | Overall 3DGS GPU scope |
| Gaussian Splat Object Cull | Object-level frustum culling |
| Gaussian Splat Sort Key Gen | Per-splat culling and depth-key generation |
| Gaussian Splat GPU Sort | Global radix sort |
| Gaussian Splat Indirect Args | Indirect draw-argument generation |
| Gaussian Splat Direct | Projection, rasterization, pixel evaluation, and blending in Direct mode |
| Gaussian Splat Accumulate | Gaussian accumulation in UE integration mode |
| Gaussian Splat Composite | Accumulation composition into SceneColor |

Android Vulkan timing must be enabled before RHI initialization:

```ini
r.Android.SupportsTimestampQueries=1
r.GPUStatsEnabled=1
```

`r.GPUStatsChildTimesIncluded=1` includes child time in parent entries. Timestamp queries add overhead and are intended for Development profiling.

To force sorting to appear every frame, temporarily run:

```text
r.GaussianSplat.ForceSortEveryFrame 1
```

Restore normal reuse after profiling:

```text
r.GaussianSplat.ForceSortEveryFrame 0
```

Leaving it enabled forces a full sort with a stationary camera or no visible Gaussians and significantly lowers FPS.

## Performance Tuning

Run controlled A/B tests from the same view:

```text
r.ScreenPercentage 50
r.GaussianSplat.OpacityAwareBounds 0
r.GaussianSplat.OpacityAwareBounds 1
r.GaussianSplat.CullMode 0
r.GaussianSplat.CullMode 2
```

Opacity-aware bounds follow the useful-support equation used by `vksplat`:

```text
extent = sqrt(2 * log(opacity / alphaThreshold))
```

The result is capped at the previous `sqrt(8)` sigma extent, so it only shrinks low-opacity oriented ellipse quads and never expands high-opacity splats. It reduces discarded fragments and overdraw in `Gaussian Splat Direct`, but does not reduce sort count.

Bottleneck indicators:

- A large Direct reduction at lower `r.ScreenPercentage` indicates fragment, bandwidth, or overdraw pressure.
- A large GPU Sort increase with forced sorting indicates global-sort pressure.
- A stationary camera being faster than a moving one indicates effective sort reuse and expensive re-sorting.
- Slow camera movement with no visible Gaussian means the renderer may still key and sort the total splat stream.

## Known Limitations and Roadmap

- Culling does not compact the global radix-sort input.
- There is no screen-contribution LOD.
- There is no tile binning or compute tile rasterizer.
- Large nearby splats can cover many pixels.
- Adreno 650 has no `R64_UINT` support; the renderer uses 32-bit keys and indices.
- Timestamp queries may be unstable on older Vulkan drivers.

Priorities are skipping sort when all objects are outside the view, compacting and sorting visible splats only, screen-space LOD, and tiled or compute rasterization.

## Troubleshooting

### Missing Cooked Content at Startup

Use the latest APK and confirm `bPackageDataInsideApk=True`.

### Vulkan ES3.1 Fallback

If logs report `VULKAN_ES3_1_ANDROID`, check whether the Device Profile sets `r.Android.DisableVulkanSM5Support=1`. Correct startup output includes:

```text
Vulkan SM5 RHI will be used!
Vulkan RHI ShaderPlatform for SM5: VULKAN_SM5_ANDROID
```

### No `stat gpu` Timing Data

Use a Development build and set `r.Android.SupportsTimestampQueries=1` through the Device Profile before startup. Setting it after RHI initialization is generally too late.
