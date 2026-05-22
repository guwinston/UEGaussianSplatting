# Model Compression

[English](Compression.md) | [简体中文](Compression.zh-CN.md)

## 1. Scope

This document focuses on the compressed runtime data format used by `Gaussian Splat Asset`, including:

- why compression is needed
- the overall structure of the current compressed format
- how each Gaussian attribute is compressed
- the compression ratio achieved by the current approach
- the main differences from SOG and SPZ
- why some attributes do not follow SOG or SPZ directly

It does not re-explain data loading, GPU sorting, or proxy-mesh shadow casting.

## 2. Compression Goals

The main problem of raw 3DGS `.ply` data is not only disk size. At runtime it also becomes strongly `memory-bound`, especially for scenes with millions to tens of millions of splats. If position, rotation, scale, opacity, and SH are kept directly as float attributes, then:

- serialized assets are large
- CPU resident memory stays large after import
- static GPU buffers become large
- shader-side decode bandwidth becomes large
- large scenes become more likely to be limited by memory bandwidth

The goal of the current compression scheme is therefore not maximum offline compression at any cost. It aims to balance:

1. efficient random runtime access to per-splat attributes
2. predictable import-time build cost
3. simple shader-side decode
4. a stable tradeoff among compression ratio, quantization error, and build time

## 3. Current Compressed Format Overview

The compressed runtime structure is defined by `FGaussianSplatCompressedData` in [GaussianSplatAsset.h](../Source/GaussianSplatting/Public/GaussianSplatAsset.h). Its main streams include:

- `PackedPositions`
- `PackedColors`
- `PackedRotations`
- `PackedScales`
- `PackedNormals`
- `PackedSHData`
- `SHCodebook`
- `ChunkPositionMins / ChunkPositionMaxs`

The overall idea is:

1. import the `.ply` and first build float-form `FGaussianSplatData`
2. apply Morton sorting to improve spatial locality
3. split the splats into fixed-size chunks and store per-chunk local bounds
4. compress `position`, `color`, `opacity`, `rotation`, `scale`, estimated normal payload, and `higher-order SH` into compact streams
5. merge compressed streams from multiple objects at runtime, upload them to the GPU, and decode in shaders as needed

So the current format is primarily a runtime-oriented compressed data layout, not a generic archival compression container.

## 4. What the Compression Ratio Is Measured Against

The reported compression ratio is not measured against `.zip` or `gzip` output. It is measured against the original floating-point attribute payload of an `Inria`-style 3DGS `PLY`.

The relevant code is:

- `EstimateInriaPlyFloatPayloadBytes`
- `EstimateCompressedRenderPayloadBytes`
- `LogCompressionStats`

The reference payload includes:

- `xyz`
- unused `normal`
- `f_dc`
- `f_rest`
- `opacity`
- `scale`
- `rotation`

So the documented compression ratio means:

> compression relative to the original floating-point 3DGS attribute data

not:

- compression relative to a `gzip` archive
- compression relative to the final packaged file size of another format

## 5. Current Compression Ratio

Using the current code path, the compressed runtime data reaches roughly `3.5x` compression relative to the original floating-point 3DGS data.

This number describes the directly renderable compressed runtime data itself. It does not add generic secondary compression such as `gzip`, and it preserves runtime random access and direct GPU decode.

## 6. Per-Attribute Compression

### 6.1 Position

Position compression uses:

1. Morton sorting
2. fixed `ChunkSize = 256`
3. per-chunk `ChunkPositionMins / ChunkPositionMaxs`
4. per-axis `uint16` quantization inside the chunk-local bounds

The relevant streams are:

- `PackedPositions`
- `ChunkPositionMins`
- `ChunkPositionMaxs`

Quantization can be written as:

$$
u=
round
\left(
65535*
\frac{p-p_{min}^{(c)}}
{p_{max}^{(c)}-p_{min}^{(c)}}
\right)
$$

and decoding as:

$$
\hat{p}=
p_{min}^{(c)}+
\frac{u}{65535}
\left(
p_{max}^{(c)}-p_{min}^{(c)}
\right)
$$

For one axis, the quantization step is:

$$
\Delta_x^{(c)}=
\frac{p_{max}^{(c)}.x-p_{min}^{(c)}.x}{65535}
$$

and the maximum absolute error bound is:

$$
|e_x^{(c)}|\le \frac{1}{2}\Delta_x^{(c)}
$$

Morton sorting helps because it makes each chunk more spatially local, which reduces the local extent and therefore reduces the position quantization step directly.

Compared with SOG, the key difference is that SOG first applies a signed logarithmic transform:

$$
q(x)=sign(x)\log(|x|+1)
$$

and then performs global 16-bit quantization in that transformed domain. The quantization is more uniform in log space, but once mapped back into linear coordinates, the absolute error grows with coordinate magnitude.

This leads to a clear difference:

- current format: local and stable absolute error controlled by chunk extent
- SOG: very high precision near the origin, but larger edge error as scene range grows

Concrete examples:

- if one chunk spans `10 m` (`1000 cm`) along one axis:

$$
|e|_{max}\le \frac{1000}{131070}\approx 0.0076\ cm
$$

- if one chunk spans `100 m` (`10000 cm`) along one axis:

$$
|e|_{max}\le \frac{10000}{131070}\approx 0.076\ cm
$$

By contrast, under a SOG-like global log quantization, a large scene can preserve extremely high precision near the center while accumulating much larger absolute error at the far edges.

### 6.2 Color and Opacity

`color` and `opacity` are packed together into `PackedColors`, one `uint32` per splat:

- `R`: 8 bit
- `G`: 8 bit
- `B`: 8 bit
- `A / Opacity`: 8 bit

`RGB` is not quantized against a fixed `[0,1]` range. The importer first computes `ColorQuantMin / ColorQuantMax` for the asset and then quantizes each channel adaptively within that range.

If a color channel is $c$ and its quantization range is $[c_{min},c_{max}]$, then:

$$
u_c=
clamp
\left(
round
\left(
255*\frac{c-c_{min}}{c_{max}-c_{min}}
\right),
0,255
\right)
$$

with decode:

$$
\hat{c}=c_{min}+\frac{u_c}{255}(c_{max}-c_{min})
$$

The channel step size is:

$$
\Delta_c=\frac{c_{max}-c_{min}}{255}
$$

and the maximum absolute error bound is:

$$
|e_c|\le \frac{c_{max}-c_{min}}{510}
$$

Opacity is quantized directly over `[0,1]`:

$$
u_\alpha=round(255\alpha),,
\hat{\alpha}=\frac{u_\alpha}{255}
$$

so the maximum absolute opacity error is:

$$
|e_\alpha|\le \frac{1}{510}\approx 0.00196
$$

### 6.3 Rotation

`rotation` is stored in `PackedRotations`, one `uint32` per splat, using `smallest-three quaternion` packing:

- 2 bit for the index of the omitted largest-magnitude component
- 10 bit + 10 bit + 10 bit for the remaining three components

If the unit quaternion is:

$$
q=(q_x,q_y,q_z,q_w),, \|q\|_2=1
$$

then the scheme:

1. finds the component with the largest absolute value
2. omits that component
3. stores the remaining three components
4. reconstructs the omitted component from the unit-length constraint

The preserved components lie in:

$$
r_i\in\left[-\frac{\sqrt{2}}{2},\frac{\sqrt{2}}{2}\right]
$$

which is why the implementation quantizes over that interval rather than over `[-1,1]`:

$$
u_i=
round
\left(
1023*
\frac{r_i+\frac{\sqrt{2}}{2}}{\sqrt{2}}
\right)
$$

and decodes as:

$$
\hat{r}_i=
\frac{\sqrt{2}}{1023}u_i-\frac{\sqrt{2}}{2}
$$

The per-component step size is:

$$
\Delta_r=\frac{\sqrt{2}}{1023}
$$

with maximum per-component error:

$$
|e_r|\le \frac{\sqrt{2}}{2046}\approx 6.91\times 10^{-4}
$$

A conservative angle-error scale is:

$$
\theta \lesssim 2.39\times 10^{-3}\ rad\approx 0.137^\circ
$$

This is not a per-asset measured error. It is a conservative scale derived from the quantization step size.

Rotation does not use a separate `codebook` mainly because:

1. the current representation already fits into a single `uint32`
2. quaternion components are strongly correlated, and `smallest-three quaternion` already exploits that structure better than a generic scalar codebook would

### 6.4 Scale

`scale` is stored in `PackedScales` using fixed `8-bit` log-scale quantization, following the same basic scale convention as SPZ.

The importer first converts source-meter scale to UE-centimeter scale in log space:

$$
s_{ue}=s_{src}+\ln(100)
$$

Then each axis is quantized independently:

$$
q=clamp
\left(
round
\left(
\frac{s_{ue}-s_{min}}{\Delta_s}
\right),
0,255
\right)
$$

with:

$$
s_{min}=-10+\ln(100),, \Delta_s=\frac{1}{16}
$$

The three `uint8` scale values are packed into one `uint32`:

$$
\texttt{PackedScale}=q_x | (q_y \ll 8) | (q_z \ll 16)
$$

Shader-side decode is:

$$
\hat{s}=s_{min}+q\Delta_s,, \hat{\sigma}=e^{\hat{s}}
$$

This gives a fixed log-space step of `0.0625`, so the maximum reconstruction error before clamping is half a step:

$$
|e_s|\le 0.03125
$$

In linear scale this is approximately:

$$
e^{0.03125}\approx 1.0317
$$

or about `3.2%` relative scale error.

Because the scale range is fixed, it also has fixed endpoints:

$$
s_{min}=-10+\ln(100),,
s_{max}=-10+\ln(100)+\frac{255}{16}=5.9375+\ln(100)
$$

Only Gaussians whose log-scale values fall inside this range are quantized with the fixed step described above. If an asset contains Gaussians outside this scale range, note that those values are clamped to the nearest endpoint.

### 6.5 Normal Payload

`PackedNormals` stores an estimated per-splat normal for future normal-aware extensions. The current renderer keeps the stream in the compressed runtime layout but does not use it for lighting.

PLY import estimates the normal from the Gaussian shape: it takes the local axis with the smallest log-scale and rotates that axis by the imported UE-space quaternion. The normal is packed with octahedral encoding into one `uint32`; currently the low 16 bits store the two 8-bit octahedral coordinates, and the upper bits are reserved for future use.

### 6.6 Higher-order SH

`PackedSHData` stores only `higher-order SH`, that is, the view-dependent SH terms beyond the DC color.

The current path:

1. computes per-asset `SHMin` and `SHMax`
2. builds a `256`-entry scalar `codebook` over that linear interval
3. quantizes each scalar SH value to one `8-bit` label
4. packs four `8-bit` labels into one `uint32`

If the scalar is $h$, the codebook is:

$$
c_k=SH_{min}+\frac{k}{255}(SH_{max}-SH_{min}),, k\in[0,255]
$$

then the quantization label is:

$$
\ell(h)=
clamp
\left(
round
\left(
255*\frac{h-SH_{min}}{SH_{max}-SH_{min}}
\right),
0,255
\right)
$$

and decode is:

$$
\hat{h}=SH_{min}+\frac{\ell(h)}{255}(SH_{max}-SH_{min})
$$

The quantization step is:

$$
\Delta_h=\frac{SH_{max}-SH_{min}}{255}
$$

so the maximum absolute error bound is:

$$
|e_h|\le \frac{SH_{max}-SH_{min}}{510}
$$

Four labels are packed into one `uint32`, which keeps decode simple and runtime-friendly.

## 7. Why Morton Sorting Is Needed

Morton sorting is not only useful for chunking and quantization. It also improves runtime memory locality.

When nearby splats in space are also nearby in the merged runtime streams:

- per-chunk position bounds become tighter
- position quantization error becomes smaller
- consecutive GPU fetches are more likely to hit nearby memory
- cache and bandwidth behavior improve

So Morton sorting helps both compression quality and runtime rendering efficiency.

## 8. Difference from SOG

### 8.1 Why the Current Path Does Not Use SOG-Style Clustering

One of the defining characteristics of the SOG route is more aggressive clustering or palette-style compression for high-dimensional attributes such as SH. From a runtime point of view, that route is meaningful: if SOG is used directly as a runtime format, it can further reduce `memory-bound` pressure.

The main reason the current implementation does not adopt that route is import-time build cost. At the scale of millions to tens of millions of splats, especially for SH-like high-dimensional data, clustering becomes too slow and makes import impractically expensive.

The current implementation therefore uses:

- direct scalar quantization for `higher-order SH`
- fixed `8-bit` log-scale quantization for `scale`

This gives up some compression ratio and some potential runtime bandwidth savings in exchange for predictable import time and a more stable build path.

## 9. Difference from SPZ

### 9.1 The Reported Compression Ratio Does Not Include `gzip`

SPZ often achieves its strongest final file-size results together with more general-purpose secondary compression such as `gzip`. The current plugin's `3.5x` figure is not measured that way. It is measured on directly renderable compressed runtime data.

So the ratio of the current format should not be compared directly against SPZ's final packaged file size as a single number.

### 9.2 Why SH Does Not Follow SPZ's Fixed `signed-byte` Path

If `gzip` is excluded and only the structured runtime payload itself is compared, SPZ at `degree 3` is about:

- `position`: `9` bytes
- `alpha`: `1` byte
- `color`: `3` bytes
- `scale`: `3` bytes
- `rotation`: `4` bytes
- `SH`: `45` bytes

for a total of about `65 bytes / splat`.

Compared with roughly `248 bytes / splat` for an original `Inria PLY`-style float payload, that gives:

$$
\frac{248}{65}\approx 3.8\times
$$

which is already close to the current format's roughly `3.5x`.

For SH precision, the current implementation is usually higher than SPZ's default settings. The current path uses a per-asset adaptive `8-bit` scalar `codebook`, while SPZ uses a fixed `[-1,1]` range and effectively stores:

- `degree 1` at `5 bit`
- `degree 2+` at `4 bit`

So without `gzip` and additional entropy-compression gains, SPZ does not have a very large file-size advantage here, while its default SH precision is usually lower.

For `scale`, the current implementation now intentionally follows SPZ's fixed log-scale byte layout, shifted by `ln(100)` because the runtime renderer works in UE centimeters. Note that this gives scale fixed lower and upper bounds; if an asset contains Gaussians outside that range, those values are clamped to the nearest endpoint.

### 9.3 Why the Current Format Is More Runtime-Oriented Than Disk-Size-Oriented

`FGaussianSplatCompressedData` strongly favors:

- fixed-stride per-splat access
- straightforward merging of multiple objects into unified GPU buffers
- direct shader-side decode by index

This is clearly a runtime-oriented design. It is not trying to minimize disk size at all costs.

That is also why the current implementation does not adopt SPZ directly: SPZ is more focused on disk compression and also adds an extra `gzip` dependency, while the current path emphasizes direct runtime upload, merging, and decoding after import.

## 10. Advantages and Costs of the Current Scheme

Main advantages:

- clear compression gains relative to the original floating-point data
- smaller runtime buffers and lower `memory-bound` pressure
- relatively stable import-time build cost
- direct decode logic that works well with merged GPU rendering
- no dependence on very slow SH `k-means`

Main costs:

- lower compression ratio than more aggressive SOG / SPZ routes
- `higher-order SH` still occupies a large fraction of the format
- the current format is optimized for runtime use, not minimum disk size

## 11. Summary

The current compression path is not trying to produce the smallest possible file. It is trying to produce a runtime-friendly format for the Unreal rendering pipeline:

- `position`: Morton sort + chunk-local `16-bit` quantization
- `color / opacity`: `8-bit` packing
- `rotation`: `smallest-three quaternion` packed into one `uint32`
- `scale`: SPZ-style fixed `8-bit` log-scale quantization
- `higher-order SH`: per-asset adaptive `8-bit` quantization plus `bit packing`

This path currently achieves about `3.5x` compression. Compared with SOG and SPZ, it gives up some maximum compression ratio, but it gains more predictable import time, a simpler runtime decode path, and a better fit for the current Unreal plugin workflow.
