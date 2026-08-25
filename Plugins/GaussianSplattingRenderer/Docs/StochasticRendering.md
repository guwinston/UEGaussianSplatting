# Stochastic Splat Rendering

[English](StochasticRendering.md) | [简体中文](StochasticRendering.zh-CN.md)

Stochastic splatting is an experimental alternative to globally sorting transparent Gaussians. It is selected with:

```text
r.GaussianSplat.SortMethod 2
```

## Pipeline

1. The key-generation pass performs the normal culling but compacts visible splat IDs directly into the final index buffer.
2. Global depth sorting is skipped.
3. The pixel shader converts Gaussian alpha into a stochastic accept/reject decision using a frame-varying sample.
4. Accepted samples write color and a private stochastic depth target, resolving visibility as opaque samples for that frame.
5. A temporal pass averages frames to reconstruct the expected transparent result.

The single-frame estimator is noisy by design. The television-snow appearance is Monte Carlo variance rather than a missing spatial denoiser. Temporal accumulation is the primary denoising mechanism.

## Temporal Accumulation

`r.GaussianSplat.StochasticTemporalSamples` controls the maximum history length:

```text
r.GaussianSplat.StochasticTemporalSamples 1000
```

A value of `0` disables history and exposes raw stochastic noise. Larger values produce a smoother stationary result but converge more slowly after changes. Once the configured sample count is reached for a stable view, the converged history can be reused.

History is reset when it is no longer valid, including camera cuts and relevant scene, view, object-descriptor, or rendering-CVar changes.

## Camera-Motion Reprojection

With reprojection enabled, VS/MS emits the screen-space motion of each splat center from the previous view to the current view. The pixel shader writes this motion to a second render target, and the temporal pass samples history at the reprojected location.

```text
r.GaussianSplat.StochasticReprojection 1
r.GaussianSplat.StochasticMotionSamples 8
```

The temporal shader also searches a 3x3 neighborhood for motion when the current pixel was rejected. If no nearby valid motion exists, it uses a short zero-motion fallback with increased current-frame weight. While the camera moves, `StochasticMotionSamples` caps the effective history length so stale samples do not dominate; when the camera stops, accumulation can converge toward the larger temporal limit again.

Set `StochasticReprojection` to `0` for an A/B test. In that mode a camera change invalidates history rather than reprojecting it.

## Geometry Paths

Stochastic rendering is independent of geometry generation:

```text
r.GaussianSplat.GeometryMode 0  # VS + PS
r.GaussianSplat.GeometryMode 1  # request Mesh Shader + PS
```

Mesh Shader mode automatically falls back to VS on unsupported RHIs.

## Performance and Memory

The path removes global depth sorting, but adds stochastic depth, temporal accumulation, and—when reprojection is enabled—a motion render target. It is therefore not guaranteed to outperform DeviceRadix in every scene.

The motion texture uses `PF_FloatRGBA`, approximately 8 bytes per pixel: about 15.8 MiB at 1920x1080 and 63.3 MiB at 3840x2160, before considering other stochastic history/depth resources and RDG aliasing.

Measure the following separately:

- key generation and compaction;
- stochastic rasterization and overdraw;
- temporal/reprojection pass;
- total GPU time during motion and after convergence.

## Suggested Tests

Raw estimator:

```text
r.GaussianSplat.SortMethod 2
r.GaussianSplat.StochasticTemporalSamples 0
```

Stationary convergence:

```text
r.GaussianSplat.StochasticTemporalSamples 1000
r.GaussianSplat.StochasticReprojection 1
```

Motion-quality A/B:

```text
r.GaussianSplat.StochasticReprojection 0
r.GaussianSplat.StochasticReprojection 1
r.GaussianSplat.StochasticMotionSamples 4
r.GaussianSplat.StochasticMotionSamples 8
r.GaussianSplat.StochasticMotionSamples 16
```

## Current Limitations

- Motion vectors represent camera-induced movement of splat centers. Independently animated or deforming Gaussian geometry is not yet reprojected from per-object motion.
- Disocclusions, very thin features, and large screen-space motion can still show noise or short trails.
- The estimator needs multiple frames; screenshots taken immediately after a cut may be noisy.
- The extra full-resolution targets can be expensive on bandwidth- and memory-constrained mobile GPUs.
- This is an experimental quality/performance tradeoff. DeviceRadix remains the default path.
