# Unreal Gaussian Splatting Renderer

[English](README.md) | [简体中文](Docs/README.zh-CN.md)

An Unreal Engine 5.5 / 5.7 plugin for importing, compressing, high-performance rendering, inspecting, and capturing 3D Gaussian Splatting scenes.

3DGS rendering in Unreal Engine is commonly implemented through three broad approaches: Niagara particle systems, custom rendering pipelines, and CUDA-backed renderers. This plugin uses a custom rendering pipeline, aiming to keep the workflow familiar to UE users while providing a faster rendering path for large Gaussian Splatting scenes.

The plugin is designed around a practical UE workflow: drag a 3DGS `.ply` file into the Content Browser to create a 3DGS asset, then drag that asset into a UE level to spawn a 3DGS Actor for rendering. From there, you can tune rendering parameters, optionally align the editor viewport to `cameras.json`, and export camera renders for comparison or dataset review.

At 1080p on an RTX 5060 Ti, measured results show that small objects with hundreds of thousands of Gaussians stay above `100 FPS`, multi-million-Gaussian scenes reach `80 FPS`, and tens-of-millions-Gaussian scenes still maintain interactive rendering above `40 FPS`.

## Features

- Import and render 3DGS-style `.ply` files
- Compress `.ply` data to reduce model storage and memory-bound rendering pressure
- DeviceRadix sorting over the GPU-generated visible count, with UE's built-in sorter as a compatibility path
- Selectable vertex-shader and mesh-shader geometry paths with automatic fallback
- Screen-size culling and opacity-aware bounds to reduce insignificant geometry and overdraw
- Experimental no-sort stochastic splatting with temporal accumulation and motion reprojection
- GPU-side splat sorting for large Gaussian scenes
- Higher-order spherical harmonics up to degree 3 for view-dependent color
- Import `cameras.json` and align the editor viewport to imported cameras
- Save the current viewport image and batch render all cameras from `cameras.json`
- Optional proxy-mesh shadow casting
- Robust occlusion handling between multiple Gaussian objects and between Gaussian objects and UE meshes
- Optional actor-level collision proxy toggle

## Requirements

- Unreal Engine 5.5 - 5.7
- Windows editor/runtime environment
- Visual Studio 2022 for C++ builds
- Input model data in 3DGS-style `.ply` format
- Optional original 3DGS `cameras.json` for camera alignment and batch capture

## Plugin Layout

```text
GaussianSplattingRenderer/
  Source/GaussianSplatting/          Runtime module
  Source/GaussianSplattingEditor/    Editor importers and details customization
  Shaders/                           Gaussian splat render shaders
  GaussianSplattingRenderer.uplugin
```

## Enable The Plugin

1. Copy `GaussianSplattingRenderer` into your project's `Plugins/` folder.
2. Open the project in Unreal Editor.
3. On first use, confirm that `GaussianSplattingRenderer` is enabled in `Edit > Plugins`.
4. Restart the editor if Unreal asks for it.
5. Build the project for `Editor` if the plugin was not already compiled.

This repository already contains the plugin in the project, so opening the `.uproject` and building the editor target is enough.

## Quick Start

![Quick start](Assets/get_start.gif)

1. Open the project in Unreal Editor.
2. Confirm that `GaussianSplattingRenderer` is enabled in `Edit > Plugins`.
3. Drag a `.ply` file into the Content Browser, or use `Import`.
4. The importer creates a `Gaussian Splat Asset`.
5. Drag the imported asset into the level.
6. Select the spawned `GaussianSplatActor`.
7. Tune the main component parameters:
   - `SplatScale`
   - `MaxSHDegree`
   - `AlphaCullThreshold`
8. If needed, use console variables to adjust global render mode, raster mode, culling behavior, and related settings.
9. View the result in the editor viewport or during play.

## Runtime Loading

You can load a `.ply` file at runtime through these Blueprint-callable APIs:

```cpp
AGaussianSplatActor::LoadFromFile(const FString& FilePath)
UGaussianSplatComponent::LoadFromFile(const FString& FilePath)
```

The path should be a valid absolute path available on the target machine.

## Camera JSON Workflow

The plugin can import the original 3DGS `cameras.json` file and use it to align the editor viewport with source training cameras.

1. Import `cameras.json` into the Content Browser.
2. Select the `GaussianSplatActor`.
3. Assign the imported camera asset to `ImportedCameraSet`.
4. Pick a camera from `SelectedImportedCamera`.
5. Use the buttons in the actor Details panel:
   - `Snap Active Viewport To Selected Camera`
   - `Save Current Viewport Image`
   - `Render All Imported Camera Images`

Batch rendering displays an editor progress dialog and can be cancelled.

### Capture Output

Captured PNG files are saved to:

```text
Saved/GaussianSplatCameraRenders
```

You can override this with `CameraRenderOutputDirectory` on the actor.

Current capture uses the active editor viewport, so the output resolution is the viewport resolution. It does not yet force the original `width` and `height` from `cameras.json`.

## Compression Overview

During import, the plugin builds compressed data used by the renderer.

Current compression behavior includes:

- Packed position, rotation, opacity, color, scale, and SH data
- Morton sorting for spatial locality
- SPZ-style fixed log-scale quantization
- Direct SH quantization and bit packing
- Compression timing and error statistics in the Unreal log

The current pipeline reaches roughly `3.5x` compression relative to the original model data. More aggressive formats such as SPZ or SOG can achieve higher compression ratios, and because they further reduce memory-bound pressure in large scenes, they can also improve rendering throughput.

This implementation does not adopt those paths wholesale for two main reasons. SPZ relies heavily on `gzip`-style general-purpose compression to reach its stronger ratios, while SOG depends on `k-means` clustering in its core compression workflow. At the scale of millions to tens of millions of Gaussians, especially for high-dimensional SH data, building those clusters becomes extremely slow during import. The scale path intentionally uses SPZ-style fixed log-scale bytes; note that this gives scale fixed lower and upper bounds, so assets with Gaussians outside that range need attention because those values are clamped to the nearest endpoint.

The current version therefore chooses a more predictable import-time path and accepts some compression-ratio loss in exchange for much more stable build times. If longer offline build times are acceptable, SPZ- or SOG-style pipelines could still push compression further and improve bandwidth-limited rendering performance in very large scenes.

## Important Actor Parameters

On `AGaussianSplatActor`:

- `SourcePlyPath`: absolute `.ply` path for direct actor loading
- `ImportedCameraSet`: imported `cameras.json` asset
- `SelectedImportedCamera`: camera used by viewport snapping
- `bApplySelectedCameraFOVToEditorViewport`: applies imported horizontal FOV while snapping
- `CameraRenderOutputDirectory`: output folder for PNG captures
- `bEnableCollisionProxy`: enables real collision from the generated selection/proxy body

`bEnableCollisionProxy` is disabled by default so Gaussian splat scenes do not block the player like an invisible solid mesh. Enable it only when you want the generated proxy body to participate in collision queries.

On `UGaussianSplatComponent`:

- `GaussianSplatAsset`: imported splat asset reference
- `SplatScale`: scales projected splat size
- `MaxSHDegree`: limits view-dependent SH evaluation
- `AlphaCullThreshold`: discards very low-opacity splats

## Console Variables

Useful runtime controls:

```text
r.GaussianSplat.GeometryMode
r.GaussianSplat.PrecomputeMode
r.GaussianSplat.PrecomputeChunkSplats
r.GaussianSplat.SortMethod
r.GaussianSplat.DeviceRadixPasses
r.GaussianSplat.DeviceRadixWriteFinalKeys
r.GaussianSplat.ScreenSizeCull
r.GaussianSplat.ScreenSizeCullMinPixels
r.GaussianSplat.OpacityAwareBounds
r.GaussianSplat.StochasticTemporalSamples
r.GaussianSplat.StochasticReprojection
r.GaussianSplat.StochasticMotionSamples
r.GaussianSplat.RasterMode
r.GaussianSplat.RenderMode
r.GaussianSplat.EnableHigherOrderSH
r.GaussianSplat.EnableAntialiasing
r.GaussianSplat.CullMode
r.GaussianSplat.SplatFrustumSlack
```

- `r.GaussianSplat.GeometryMode`
  `0` uses VS + PS; `1` requests Mesh Shader + PS and automatically falls back to VS when mesh shaders are unavailable.
- `r.GaussianSplat.PrecomputeMode`
  Selects the projection path. `0` keeps the legacy six-vertex path and repeats projection work in the VS; `1` precomputes one screen-space record per visible splat and keeps the six-vertex draw. Stochastic no-sort rendering is supported: its precomputed record adds the previous-frame reprojection vector required by temporal accumulation. The default remains `0` for an unchanged baseline.
- `r.GaussianSplat.PrecomputeChunkSplats`
  Caps the reusable preprojection buffer and interleaves compute/draw chunks without changing draw order. The default `1048576` uses 64 MiB normally and 80 MiB with stochastic reprojection; values are clamped to 65536..4194304. Set `0` to restore a monolithic full-cloud allocation for memory/performance A/B tests.
- `r.GaussianSplat.SortMethod`
  `0` uses UE `SortGPUBuffers`, `1` uses DeviceRadix over the compacted visible count (default), and `2` enables experimental no-sort stochastic rendering.
- `r.GaussianSplat.DeviceRadixPasses`
  Selects `1` to `4` 8-bit radix passes. Four passes preserve the full 32-bit ordering key; fewer passes trade ordering precision for sort cost.
- `r.GaussianSplat.DeviceRadixWriteFinalKeys`
  Defaults to `0` because rasterization consumes sorted indices only. Enable final key output only for diagnostics or a downstream consumer.
- `r.GaussianSplat.ScreenSizeCull` / `r.GaussianSplat.ScreenSizeCullMinPixels`
  Discards splats whose projected extent is below the configured pixel threshold. The defaults are enabled and `1.0` pixel.
- `r.GaussianSplat.OpacityAwareBounds`
  Shrinks low-opacity splat bounds to reduce fragments and overdraw without expanding the original support radius.
- `r.GaussianSplat.StochasticTemporalSamples`
  Sets the temporal accumulation limit for stochastic rendering. `0` disables history; the default is `1000`.
- `r.GaussianSplat.StochasticReprojection`
  Reprojects stochastic history using per-splat camera motion. Enabled by default.
- `r.GaussianSplat.StochasticMotionSamples`
  Caps effective history while the camera moves to limit ghosting. The default is `8`.
- `r.GaussianSplat.RasterMode`
  `0` uses unit-circle pixel evaluation, while `1` uses conic / CUDA-like pixel evaluation.
- `r.GaussianSplat.RenderMode`
  `0` composites before tonemapping through an accumulation target, while `1` blends after tonemapping and is intended to match the original 3DGS rendering result.
- `r.GaussianSplat.EnableHigherOrderSH`
  Set this to `0` to disable higher-order SH evaluation and use constant color only.
- `r.GaussianSplat.EnableAntialiasing`
  Enables Mip-Splatting style opacity compensation for the covariance low-pass filter. This can reduce aliasing and popping on small or distant splats.
- `r.GaussianSplat.CullMode`
  `0` disables culling, `1` enables object-level culling only, and `2` enables object-level plus per-splat XY frustum culling.
- `r.GaussianSplat.SplatFrustumSlack`
  Adjusts the slack factor used by per-splat frustum culling to reduce accidental clipping near the viewport edges.

For an in-process A/B comparison, keep the camera, resolution, sort settings, and
`r.GaussianSplat.ForceSortEveryFrame` value fixed, then switch only the precompute mode:

```text
r.GaussianSplat.PrecomputeMode 0
r.GaussianSplat.PrecomputeMode 1
r.GaussianSplat.PrecomputeChunkSplats 1048576
stat gpu
```

The GPU profiler exposes the extra compute stage as `Gaussian Splat Precompute`.
Compare total `Gaussian Splat` GPU time rather than raster time alone because mode
`1` deliberately moves projection work out of the vertex shader.

## Selection And Collision Proxy

The plugin stores a generated selection/proxy body so translucent Gaussian splats can still be selected and inspected in the editor. When a `ShadowProxyMesh` is configured, the proxy can also be used as an approximate shadow caster for the Gaussian model.

The proxy body may come from:

- an assigned `ShadowProxyMesh`, or
- generated selection hull data stored on the Gaussian splat asset.

By default this proxy is not real gameplay collision. Use `bEnableCollisionProxy` on the actor if you want it to block or answer collision queries. If a `ShadowProxyMesh` is assigned, that proxy mesh can also serve as an approximate shadow-casting representation.

## Data Flow

The main runtime data flow is:

1. Import the source `.ply` into the Content Browser and build a `UGaussianSplatAsset`, including compressed Gaussian attribute data generated during import.
2. Drag the generated asset into a level to create an `AGaussianSplatActor` and its `UGaussianSplatComponent`, which read the compressed asset data at runtime.
3. Merge compressed data from multiple Gaussian objects into the global buffer layout required by the renderer, then upload those buffers to the GPU.
4. Inside the `SceneViewExtension`, combine the current view, global CVars, object state, and visibility data to prepare per-frame render parameters and GPU resources.
5. Compact visible splats. The default DeviceRadix path sorts only the GPU visible count; stochastic mode skips sorting and writes visible indices directly.
6. Generate indirect arguments and rasterize through VS + PS or, when supported and selected, Mesh Shader + PS.
7. In stochastic mode, temporally accumulate accepted samples and optionally reproject history using per-splat camera motion.
8. Composite with UE scene rendering so Gaussian objects occlude one another and interact correctly with regular UE meshes.

## Detailed Docs

For a deeper explanation of the rendering math and shader-side implementation details, see:

- [Rendering Principles](Docs/RenderingPrinciples.md)
- [GPU Sorting](Docs/GPUSorting.md)
- [Rendering Paths and Performance](Docs/RenderingPaths.md)
- [Stochastic Rendering](Docs/StochasticRendering.md)
- [Compression](Docs/Compression.md)
- [Mobile Platform Support](Docs/Mobile.md)

This document covers:

- source 3DGS to UE coordinate conversion
- 3D Gaussian projection into a 2D ellipse
- VS / Mesh Shader / PS responsibility split
- the equivalent `RASTER_MODE=0/1` evaluation paths
- alpha blending and sort direction
- occlusion between Gaussian objects and between Gaussian objects and UE meshes
- the optional proxy shadow-casting path
- the merged global GPU sorting path, including key/value layout and indirect draw generation
- the current compressed runtime data format, attribute packing strategy, and the tradeoffs against SOG and SPZ

## References

- [Official 3D Gaussian Splatting repository](https://github.com/graphdeco-inria/gaussian-splatting)
- [SuperSplat](https://github.com/playcanvas/supersplat)
- [SPZ](https://github.com/nianticlabs/spz)
- [vk_gaussian_splatting](https://github.com/nvpro-samples/vk_gaussian_splatting)

## License / Distribution

This plugin is released under the MIT License. See the `LICENSE` file in the plugin directory for details.
