# Rendering Paths and Performance

[English](RenderingPaths.md) | [简体中文](RenderingPaths.zh-CN.md)

The renderer exposes two independent choices: how visible splats are ordered and how each visible splat generates its quad. There is no compute-generated geometry path in the current implementation.

## Path Selection

| Control | Value | Path | Notes |
| --- | --- | --- | --- |
| `r.GaussianSplat.SortMethod` | `0` | UE built-in sort | Compatibility/reference path; sorts the allocated full stream |
|  | `1` | DeviceRadix | Default; compacts and sorts only the GPU visible count |
|  | `2` | Stochastic | Experimental; compacts visible IDs and skips depth sorting |
| `r.GaussianSplat.GeometryMode` | `0` | VS + PS | Broadest compatibility |
|  | `1` | Mesh Shader + PS | Default request; automatically falls back to VS when unsupported |

The two controls can be combined. For example, DeviceRadix + Mesh Shader is the normal high-performance desktop path, while DeviceRadix + VS is the expected path on Vulkan SM5 mobile hardware without mesh-shader support.

## Vertex Shader Path

The VS path draws an indirect indexed quad for every visible splat. Each vertex reads the splat ID, loads compressed attributes, projects the 3D covariance to a 2D ellipse, and emits a quad corner. It is simple and widely supported, but repeated work may occur across the quad vertices.

Use it explicitly with:

```text
r.GaussianSplat.GeometryMode 0
```

## Mesh Shader Path

The mesh path dispatches work indirectly and lets a mesh-shader workgroup generate splat primitives. It can share per-splat setup more efficiently and avoid parts of the traditional vertex/index pipeline. The actual gain depends on GPU architecture, driver, splat count, screen coverage, and whether the frame is limited by sorting or fragment overdraw.

Request it with:

```text
r.GaussianSplat.GeometryMode 1
```

This is a request rather than a guarantee. The renderer checks RHI mesh-shader support and uses the VS path when unavailable. Most current mobile targets should be treated as VS-only even when running Vulkan SM5.

## Culling Before Sorting and Rasterization

The key-generation pass can combine:

- object-level frustum culling;
- per-splat XY frustum culling;
- screen-size culling for sub-pixel splats;
- opacity-aware raster bounds that shrink low-opacity ellipse quads.

Relevant controls:

```text
r.GaussianSplat.CullMode 2
r.GaussianSplat.SplatFrustumSlack 1.3
r.GaussianSplat.ScreenSizeCull 1
r.GaussianSplat.ScreenSizeCullMinPixels 1.0
r.GaussianSplat.OpacityAwareBounds 1
```

Screen-size culling reduces the visible count before DeviceRadix sorting and indirect rendering. Opacity-aware bounds primarily reduce rasterized fragments and overdraw; they do not remove an otherwise visible splat from sorting.

## Recommended Presets

Desktop sorted path:

```text
r.GaussianSplat.SortMethod 1
r.GaussianSplat.GeometryMode 1
r.GaussianSplat.DeviceRadixPasses 4
r.GaussianSplat.DeviceRadixWriteFinalKeys 0
```

Compatibility/reference path:

```text
r.GaussianSplat.SortMethod 0
r.GaussianSplat.GeometryMode 0
```

Experimental stochastic path:

```text
r.GaussianSplat.SortMethod 2
r.GaussianSplat.GeometryMode 1
r.GaussianSplat.StochasticTemporalSamples 1000
r.GaussianSplat.StochasticReprojection 1
r.GaussianSplat.StochasticMotionSamples 8
```

## Benchmarking Correctly

Compare paths from the same camera, resolution, scene, render mode, and warm state. Record `stat unit` and `stat gpu`, not FPS alone. Sorting is normally reused while the camera is stationary, so a static-view comparison may hide sort cost.

Temporarily force sorting while profiling sorted modes:

```text
r.GaussianSplat.ForceSortEveryFrame 1
```

Restore it afterward:

```text
r.GaussianSplat.ForceSortEveryFrame 0
```

Interpret the result by stage: a faster mesh path will not materially change a frame dominated by pixel overdraw, and a faster sorter will have little effect when sort results are reused. Always validate visual equivalence when reducing DeviceRadix pass count or increasing the screen-size threshold.
