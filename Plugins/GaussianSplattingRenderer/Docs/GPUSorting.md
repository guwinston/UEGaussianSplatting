# GPU Sorting

[English](GPUSorting.md) | [简体中文](GPUSorting.zh-CN.md)

This document describes the current GPU sorting path used by the plugin, including sorting goals, data structures, the sorting flow, and the design rationale behind it.

It does not re-explain:

- projection of 3D Gaussians into 2D ellipses
- proxy-mesh shadow casting
- `.ply` import and compression build steps

## 1. Sorting Goal

The current rendering path uses back-to-front alpha blending. That requires all splats participating in translucent accumulation to be ordered from far to near in the current view.

For a single Gaussian object, sorting only inside that object would be enough. The plugin, however, supports multiple Gaussian objects contributing to the same final view and requires correct occlusion:

- between Gaussian objects
- between Gaussian objects and UE meshes

The goal is therefore not object-local sorting, but a single global sort over all splats participating in the current view.

## 2. Sorting System Overview

The current sorting path consists of three parts:

1. merged global static splat buffers
2. per-frame GPU culling and depth-key generation
3. a global GPU sort performed through UE's built-in GPU sorting interface

The renderer does not reorder full splat attribute buffers every frame. Instead, it produces a sorted global splat index stream, and the vertex shader uses that stream to fetch the corresponding splat attributes from static buffers.

Relevant code:

- [GaussianSplatViewExtension.cpp](../Source/GaussianSplatting/Private/GaussianSplatViewExtension.cpp)
- [GaussianSplatSorter.cpp](../Source/GaussianSplatting/Private/GaussianSplatSorter.cpp)
- [GaussianSplatCullAndSortKeyGen.usf](../Shaders/GaussianSplatCullAndSortKeyGen.usf)

## 3. Sorting Infrastructure

The plugin does not implement a full standalone GPU sorter from scratch. It reuses UE's GPU sorting interface:

```cpp
SortGPUBuffers(...)
```

The plugin side is responsible for:

- preparing key/value buffers
- object-level and per-splat culling
- maintaining the visible splat count
- generating indirect draw arguments

UE is responsible for the actual GPU sort.

The relevant UE implementation is in `GPUSort.h` and `GPUSort.cpp`. That sorter uses GPU **radix sort** and explicitly defines:

```cpp
#define RADIX_BITS 4
```

This does **not** mean that only `4 bit` are sorted. It means each pass processes `4 bit`. For a `uint32` key, the full sort can therefore require up to:

$$
32 / 4 = 8
$$

passes.

## 4. Sorting Data Structures

### 4.1 Global Splat Index Stream

After static data is merged, every splat has a unique `global splat index`. That index addresses:

- global position buffer
- global color buffer
- global rotation buffer
- global scale buffer
- global normal buffer
- global SH buffer

The sorter reorders indices, not the full attribute payload.

### 4.2 Sorting Key

The key is built from the splat center depth in view space:

$$
k_i=SortableUint(-z_i)
$$

where:

- $z_i$ is the view-space depth of splat $i$
- `SortableUint` converts a float depth into a sortable unsigned-integer representation

See [GaussianSplatCullAndSortKeyGen.usf](../Shaders/GaussianSplatCullAndSortKeyGen.usf):

```hlsl
OutDepthKeys[globalSplatIndex] = FloatToSortableUint(-viewPos.z);
```

This directly matches the back-to-front compositing target.

### 4.3 Sorting Value

The value is simply the global splat index itself:

$$
v_i=i,, i\in[0,N-1]
$$

See `EnsureIdentityIndexBuffer` in [GaussianSplatSorter.cpp](../Source/GaussianSplatting/Private/GaussianSplatSorter.cpp).

After sorting, the important result is not the reordered key stream but the reordered value stream, which becomes the final:

- `SortedVisibleIndexBuffer`

used by rendering.

## 5. Why the Key / Value Layout Looks Like This

The design follows two principles:

1. keep the sorting input minimal
2. avoid reordering static attribute buffers every frame

Only depth is needed for the current blending order, so the key stores only depth-related ordering information. Including color, rotation, scale, or other attributes in the key would not improve transparent compositing correctness.

Likewise, full splat attributes are much larger than a single index. Reordering full attribute buffers every frame would be much more expensive than sorting a `uint -> uint` key/value stream and then using the sorted index stream to fetch static data.

This layout also matches UE's `SortGPUBuffers` interface naturally, since that interface is built around key/value streams rather than arbitrary large structs.

## 6. Culling and Key Generation

Before sorting, the implementation performs two GPU culling stages.

### 6.1 Object-Level Culling

The first stage tests each object's local bounds conservatively against the frustum and writes the result into `ObjectVisibilityBuffer`. If the whole object is invisible, all of its splats can be skipped quickly in the next stage.

### 6.2 Per-Splat Culling

The second stage iterates over the global splat stream and checks:

1. object-level visibility
2. opacity threshold
3. whether the splat center is in front of the camera
4. optional per-splat XY frustum conditions

If a splat survives, the shader:

- increments `VisibleCount`
- writes a normal depth key

If a splat fails, the shader writes:

```hlsl
0xFFFFFFFFu
```

as the key, pushing that splat to the tail of the sorted stream. Later, the indirect draw only consumes the first `VisibleCount` splats, so those tail entries are never rasterized.

## 7. GPU Sorting Pass

After culling and key generation, the implementation hands the following to UE's `SortGPUBuffers`:

- ping-pong key buffers
- ping-pong value buffers
- the identity value stream
- the final output value buffer

The key expresses the current view-dependent ordering. The value expresses splat identity. The final sorted output is therefore a reordered global splat index stream.

## 8. Indirect Draw Argument Generation

The visible splat count is not read back to the CPU. Instead, the GPU builds the indirect draw arguments directly from `VisibleCountBuffer[0]`.

Each billboard quad is made of two triangles, so each splat needs 6 vertices:

$$
VertexCount=VisibleCount\times 6
$$

See [GaussianSplatCullAndSortKeyGen.usf](../Shaders/GaussianSplatCullAndSortKeyGen.usf):

```hlsl
OutDrawIndirectArgs[0] = visibleCount * 6u;
```

The raster pass therefore renders only the first `VisibleCount` splats in the sorted stream.

## 9. How Multi-Object Transparent Occlusion Works

The plugin does not sort and draw each Gaussian object independently. Instead, all splats from all objects are merged into one global stream, globally sorted, and rendered through a merged draw call.

This is the key to correct multi-object transparency. If each object were sorted and drawn independently, only intra-object ordering would be correct. Splats from object A and object B could not interleave correctly in global depth order.

The merged global sort and merged draw solve both:

- correct transparent occlusion between Gaussian objects
- low draw-call overhead

## 10. Relevant Parameters

The main sorting-related global controls are:

- `r.GaussianSplat.CullMode`
  - `0`: no culling
  - `1`: object-level culling only
  - `2`: object-level plus per-splat XY frustum culling

- `r.GaussianSplat.SplatFrustumSlack`
  - controls the slack used by per-splat frustum tests

These affect:

- how many splats enter the visible prefix
- how much useful work the sorter needs to process
- the tradeoff between aggressive culling and accidental edge clipping

## 11. Summary

The GPU sorting path can be summarized as:

**merge all splats into one global stream; use view-space depth as the key and the global splat index as the value; reuse UE's GPU radix sort to sort that stream; and then drive one merged indirect draw from the visible sorted prefix.**

The key characteristics are:

- the sorted entity is the global splat stream, not an object-local stream
- keys and values are clearly separated
- the sorter itself is UE's built-in GPU radix sort
- the result supports both correct multi-object transparency and draw-call reduction
