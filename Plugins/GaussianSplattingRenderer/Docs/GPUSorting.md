# GPU Sorting

[English](GPUSorting.md) | [简体中文](GPUSorting.zh-CN.md)

This document describes the current ordering paths used by `GaussianSplattingRenderer`. The renderer merges visible Gaussian objects into one global splat stream so transparent ordering is correct across object boundaries.

## 1. Why Sorting Is Needed

The normal renderer uses source-alpha blending and draws splats back to front. Sorting each actor independently is insufficient: splats from two overlapping actors must participate in the same view-dependent order.

The experimental stochastic path is the exception. It replaces ordered alpha blending with probabilistic coverage, a private depth target, and temporal reconstruction.

## 2. Per-Frame Overview

When an ordered path needs a new sort, the GPU performs:

1. object-level visibility testing;
2. per-splat culling and 32-bit depth-key generation;
3. visible key/value compaction when DeviceRadix is active;
4. GPU sorting;
5. indirect argument generation from the GPU visible count;
6. VS or Mesh Shader rasterization in sorted order.

A stationary view normally reuses the previous result. Set `r.GaussianSplat.ForceSortEveryFrame 1` only while measuring sort cost, then restore it to `0`.

## 3. Sort Methods

`r.GaussianSplat.SortMethod` selects the path:

| Value | Path | Input size | Purpose |
| --- | --- | --- | --- |
| `0` | UE `SortGPUBuffers` | Allocated total splat count | Compatibility and reference |
| `1` | DeviceRadix | GPU visible count | Default high-performance ordered path |
| `2` | Stochastic no-sort | No depth sort | Experimental quality/performance tradeoff |

If DeviceRadix is unavailable on the active RHI, method `1` falls back to the UE built-in sorter.

### 3.1 UE Built-In Sort

The compatibility path writes one key/value entry for every allocated splat. Culled entries receive a sentinel key and move to the tail, but `SortGPUBuffers` still processes `TotalSplatCount`. This is useful as a correctness baseline, but culling does not reduce the radix workload.

No engine source is modified. The plugin calls the public renderer infrastructure as provided by UE.

### 3.2 DeviceRadix Visible-Count Sort

The default path atomically compacts visible key/value pairs into the prefix `[0, VisibleCount)`. The visible count remains on the GPU and is passed to DeviceRadix as its active element count; there is no CPU readback or frame stall.

Buffer capacity is still sized for the total splat count, but radix histogram, prefix, and scatter work only cover the active visible prefix. Consequently, object culling, per-splat frustum culling, and screen-size culling can directly lower sort work.

DeviceRadix uses configurable 8-bit least-significant-digit passes:

```text
r.GaussianSplat.DeviceRadixPasses 4
r.GaussianSplat.DeviceRadixWriteFinalKeys 0
```

The pass count is clamped to `1..4`. Four passes sort the complete 32-bit key. Fewer passes start at `32 - PassCount * 8`, reducing cost but also depth-order precision. The final sorted keys are disabled by default because rasterization only consumes sorted values/indices.

### 3.3 Stochastic No-Sort

Method `2` compacts visible IDs directly into the final index buffer and skips global depth sorting. Visibility is resolved by stochastic acceptance and depth during rasterization, then temporally accumulated. See [Stochastic Rendering](StochasticRendering.md).

## 4. Key and Value Layout

Each ordered entry contains:

- a 32-bit sortable depth key;
- a 32-bit global splat index.

The index addresses the merged attribute buffers, so a single sorted stream can contain splats from every visible Gaussian object. The key transform preserves the required far-to-near order for alpha blending.

## 5. Culling and Compaction

The key-generation compute pass can apply:

- object-level frustum culling;
- per-splat XY frustum culling;
- a configurable frustum slack factor;
- projected screen-size culling.

Main controls:

```text
r.GaussianSplat.CullMode 2
r.GaussianSplat.SplatFrustumSlack 1.3
r.GaussianSplat.ScreenSizeCull 1
r.GaussianSplat.ScreenSizeCullMinPixels 1.0
```

With DeviceRadix, rejected splats do not enter the active prefix. With the built-in sorter they remain allocated sentinel entries. With stochastic rendering, accepted visible IDs are compacted directly into the draw index buffer.

`r.GaussianSplat.OpacityAwareBounds` is related but acts later: it shrinks the raster support of low-opacity splats and reduces fragment overdraw, not sort count.

## 6. Indirect Arguments

The GPU visible count generates both drawing forms without CPU synchronization:

- indirect indexed-draw arguments for VS + PS;
- indirect mesh-dispatch arguments for Mesh Shader + PS.

This keeps culling, sorting, and draw submission GPU-driven. Geometry-path selection is documented in [Rendering Paths and Performance](RenderingPaths.md).

## 7. Performance Guidance

Use DeviceRadix (`SortMethod 1`) as the normal ordered path. Use the built-in sorter only for compatibility or A/B validation. Consider fewer DeviceRadix passes only after checking transparency quality in difficult overlapping views.

When profiling, keep the camera, resolution, render mode, culling settings, and geometry mode fixed. Measure the key-generation and sort scopes separately: a lower visible count helps DeviceRadix, while a fragment-bound view may show little total-frame improvement even when sorting is faster.

## 8. Summary

- Sorting is global across Gaussian objects.
- DeviceRadix is the default and sorts only the GPU-generated visible count.
- The UE built-in path sorts the full allocated stream and remains a compatibility baseline.
- Stochastic mode skips global depth sorting and relies on temporal reconstruction.
- No CPU readback is required for active counts or indirect arguments.
- No Unreal Engine source changes are required.
