# Rendering Principles

[English](RenderingPrinciples.md) | [简体中文](RenderingPrinciples.zh-CN.md)

This document focuses only on the rendering path used by the plugin, including:

- coordinate-system conventions for 3DGS data inside the current renderer
- projection of a 3D Gaussian from 3D space into 2D screen space
- the work done by the vertex shader and pixel shader
- alpha evaluation, alpha blending, and final compositing

## 1. Overall Rendering Path

The rendering path in this plugin is essentially answering one question: how can an anisotropic Gaussian defined in 3D space be pushed through a conventional graphics pipeline so that it can be projected, rasterized, and blended? A standard graphics pipeline cannot directly "draw a 3D Gaussian distribution", because the vertex shader must eventually output rasterizable primitives, and the pixel shader can only evaluate pixels covered by those primitives. Therefore, the current implementation first projects each 3D Gaussian into screen space, obtains the corresponding 2D elliptical distribution under the current view, builds a billboard quad that encloses that ellipse, lets the GPU rasterize the quad as two triangles, and finally evaluates the actual 2D Gaussian only for the pixels covered by that billboard.

In the current implementation, the rendering path can be summarized as follows:

1. CPU-side code prepares per-splat position, color, rotation, scale, SH, and related data, then uploads it to the GPU.
2. The GPU vertex shader decodes the current splat, transforms its center from local space into view space, and reconstructs the 3D covariance from rotation and scale.
3. The 3D covariance is projected into screen space through the projection Jacobian of the current view, producing a 2D covariance that describes the screen-space shape of the splat.
4. The 2D ellipse is decomposed into two screen-space principal axes, and those axes are used to construct a billboard quad that encloses the ellipse.
5. After rasterization, the pixel shader continues only for pixels covered by the billboard. It evaluates the 2D Gaussian weight at each pixel and multiplies that weight by the splat's base opacity to obtain the final pixel alpha.
6. The pixel color and alpha are then accumulated with back-to-front alpha blending, producing the final image.

So the path does not draw a traditional point, sphere, or explicit mesh. It draws the 2D elliptical distribution obtained by projecting a 3D Gaussian into the current view. The vertex shader turns that distribution into a rasterizable billboard, while the pixel shader reconstructs the Gaussian distribution inside the billboard and relies on alpha blending to form the final image. This keeps the rendering principle aligned with the official 3DGS rendering path.

## 2. From 3DGS Coordinates to UE Coordinates

The original 3DGS data does not use the same coordinate system as Unreal Engine. The plugin therefore defines a fixed source-to-UE conversion rule. This is an engineering convention whose purpose is to express positions, directions, rotations, covariances, camera poses, and SH view directions under one consistent UE basis. There is no single universally correct mapping; the important part is that the whole pipeline stays self-consistent.

The convention used by the current implementation is:

```text
3DGS/COLMAP = ( X,  Y,  Z )
    UE      = ( Z,  X, -Y )
```

That means:

- 3DGS `X` maps to UE `Y`
- 3DGS `Y` maps to UE `-Z`
- 3DGS `Z` maps to UE `X`

This mapping mainly unifies handedness and axis definitions. Other mappings could also work, such as mirroring another axis or swapping different axes, as long as the model, cameras, and shader-side interpretation all use the same convention. The key is not the specific mapping itself, but that every spatial attribute is interpreted with that same mapping.

### 2.1 Position Conversion

In the current code, position conversion is written directly as:

```cpp
FVector(
    SourcePositionMeters.Z * 100.0,
    SourcePositionMeters.X * 100.0,
    -SourcePositionMeters.Y * 100.0);
```

This performs two things at once:

1. axis remapping and sign flipping, converting the source basis into the UE basis
2. unit conversion from meters to centimeters, because 3DGS / COLMAP data is usually expressed in meters while UE world units are centimeters

For position alone, this can be viewed as:

$$
p_{ue} = 100 \, M \, p_{src}
$$

where `M` is the 3x3 axis mapping matrix chosen by the project.

### 2.2 Direction and Camera Basis Conversion

Direction vectors do not need unit scaling, but they must use the exact same axis convention as positions. More precisely, conversion is not just copying the three components of a vector into a different order. It first maps what each source axis means into UE basis semantics, and then expresses the original vector under that new basis.

Under the 3DGS / COLMAP convention, the source axes can be understood as:

- `X`: Right
- `Y`: Down
- `Z`: Forward

Under common UE local-space semantics:

- `X`: Forward
- `Y`: Right
- `Z`: Up

So the actual procedure has two conceptual steps:

1. Convert the source basis vectors themselves into the UE basis.
2. Re-express positions, directions, camera orientation, and related quantities under the new basis.

The current direction conversion from source coordinates to UE coordinates is:

$$
d_{ue} =
(d_{src,z}, d_{src,x}, -d_{src,y})^T
$$

This means:

- source `Forward(Z)` becomes UE `Forward(X)`
- source `Right(X)` becomes UE `Right(Y)`
- source `Down(Y)` becomes UE `-Up(Z)`

The `cameras.json` import follows this principle. `cameras.json` stores a camera-to-world rotation matrix whose three columns in the source coordinate system represent:

- `col0`: camera Right
- `col1`: camera Down
- `col2`: camera Forward

Therefore, the import logic does not directly treat that 3x3 matrix as a UE rotation matrix. Instead, it converts the three basis vectors into UE space first, then rebuilds the UE camera pose using UE camera semantics:

- `Forward` is obtained by converting the source-space `Forward` vector into UE
- `Up` is obtained by converting the source-space `Down` vector into UE and then negating it
- `FRotationMatrix::MakeFromXZ()` is then used to rebuild the rotation with UE's `X=Forward`, `Z=Up` camera convention

As a result, imported `cameras.json` cameras and imported Gaussian models use the same coordinate convention. Viewport alignment, batch rendering, and later camera flythrough workflows can therefore preserve correct spatial relationships.

### 2.2.1 UE Camera X Forward vs Render-Space Z Forward

There is an easy point to misunderstand here: when we say a UE camera is `X Forward`, that does not mean UE uses world-space `X` directly as the projection depth axis all the way through rendering.

From the engine source, when UE builds the view matrix, it first applies `FInverseRotationMatrix(ViewRotation)` and then right-multiplies a fixed axis-remapping matrix:

```cpp
FMatrix(
    FPlane(0, 0, 1, 0),
    FPlane(1, 0, 0, 0),
    FPlane(0, 1, 0, 0),
    FPlane(0, 0, 0, 1))
```

This step effectively changes the usual UE world/local semantics:

```text
X = Forward
Y = Right
Z = Up
```

into the view-space semantics expected by projection and clip-space handling:

```text
View X = Right
View Y = Up
View Z = Forward
```

In other words:

- at the editor and gameplay level, UE cameras do face `+X`
- after entering render view space, the forward depth axis is arranged as `+Z`
- the later perspective projection matrix works with view-space `Z` as the forward/depth component

This can also be seen in the engine's perspective projection matrix definition: the rows that determine depth mapping operate on the view-space `Z` component. Therefore, for this plugin, the more accurate description is:

- the first conversion layer maps source 3DGS data into UE world/local semantics
- before rendering, UE applies a second conversion layer from `X Forward / Y Right / Z Up` world semantics into `X Right / Y Up / Z Forward` view semantics

With this two-layer interpretation, imported Gaussian positions, camera poses, and covariance directions remain consistent after entering `WorldToView` and `ViewToClip`.

### 2.3 Scale, Rotation, and Covariance Conversion

A 3DGS splat is not just a center point. It is also defined by scale and rotation, which together describe a 3D anisotropic Gaussian. In other words, the object being projected is not "position + radius", but a full 3D covariance matrix.

Under the current coordinate convention, `scale` and `rotation` must be interpreted in the UE basis just like positions and directions. For `scale`, in addition to matching the axis semantics carried by the rotation, the unit system also matters: 3DGS / COLMAP data is usually in meters, while UE uses centimeters.

Since the stored value is not linear scale but `log-scale`, the unit conversion is not simply a direct linear-space operation written as `s -> 100s`. In log space, it becomes a constant offset:

$$
\log(100 s) = \log s + \log 100
$$

More completely:

$$
s_{ue} = 100 \, s_{src}
$$

and under `log-scale` representation:

$$
\log s_{ue} = \log s_{src} + \log 100
$$

This means the log-scale values remain continuous and stable after switching to UE units. The meter-to-centimeter conversion does not inherently break the precision behavior of the current `log-scale` compression.

For `rotation`, the current code does not rely only on an abstract change-of-basis matrix formula. It uses a direct procedure:

1. Reconstruct the source-space local rotation matrix from the source quaternion.
2. Extract the local `X` and `Y` axes, which are two local principal directions of the splat in source space.
3. Convert those two directions into UE using the same source-to-UE direction mapping:

$$
d_{ue} =
(d_{src,z}, d_{src,x}, -d_{src,y})^T
$$

4. Rebuild the UE rotation from the converted UE local `X` and `Y` axes.

So the code is effectively doing this:

- recover the source-space local basis from the source quaternion
- convert each local basis direction into UE space
- rebuild a UE quaternion from those UE-space local axes

This does not mean the Gaussian is being rotated again. It means the same local orientation is being re-expressed under UE coordinate semantics. Since the three scale components are defined along that local basis, once the local rotation has been correctly re-expressed in UE, `scale_x / scale_y / scale_z` still attach to their corresponding local axes. The scale components do not need to be additionally permuted; only the unit conversion matters.

Once `rotation` and `scale` are both interpreted under UE semantics, the corresponding 3D covariance can be written as:

$$
\Sigma_{3D} = R_{ue} \, S_{ue}^{2} \, R_{ue}^{T}
$$

Here $R_{ue}$ is the rotation matrix under the UE basis, and $S_{ue}$ is the axis-aligned scale matrix under UE semantics. Together, they define the 3D ellipsoid shape of the splat in UE space. Later view-space transformation and projection-Jacobian mapping both operate on this already unified UE covariance.

### 2.4 SH Coefficients and View-Direction Conversion

For splats with higher-order SH, color is not fixed. It is evaluated from the viewing direction. The important detail is that the SH coefficients are still interpreted under the original 3DGS SH basis. Therefore, in the shader, the view direction used for SH evaluation must be mapped back into the original 3DGS basis.

Before SH evaluation, the current implementation maps the UE view direction back to 3DGS direction space:

$$
v_{3dgs} =
(v_{ue,y}, -v_{ue,z}, v_{ue,x})^T
$$

This is the inverse of the earlier `UE = (Z, X, -Y)` mapping. The reason is simple: the model has been placed in UE space, but the SH coefficients are still stored in the original 3DGS direction semantics. If the evaluation direction is not mapped back, view-dependent color will be rotated incorrectly. This shows up as incorrect highlight direction, shifted brightness response, or wrong angular color dependence.

There is also an engineering tradeoff here: why does the implementation keep the original SH coefficients and transform only the view direction, instead of converting all SH coefficients into the UE basis at import time?

There are two main reasons.

First, converting the view direction is extremely cheap. Each evaluation only touches one 3D direction vector, and the conversion itself is just a fixed component shuffle plus sign flip.

Second, rotating SH coefficients is much more complex. SH coefficients are not ordinary vectors. When the SH basis is rotated, coefficients of the same band can couple with each other, and each band needs the correct rotation transform. At the same time, the number of SH coefficients is much larger than a single view direction, especially across every splat and every color channel. Doing this during import or compression would add implementation complexity and extra data processing.

So from an implementation standpoint, keeping the original SH coefficients and mapping the evaluation direction back to the 3DGS basis at runtime is simpler, cheaper, and easier to verify. In this part of the pipeline, what is transformed is not the SH coefficient array itself, but the direction vector used to evaluate it.

### 2.5 Why These Conversions Must Stay Consistent

If position is converted but direction, camera pose, covariance, or SH evaluation direction is not converted consistently, the result will have several independent but obvious problems:

- the model appears in the right place, but ellipses have the wrong orientation
- cameras approximately line up, but FOV or orientation does not match training views
- screen-space major and minor axes differ from the original result
- SH view-dependent color is rotated incorrectly
- imported `cameras.json` views do not strictly align with the model

So this section is not merely saying that position is converted to `(Z, X, -Y)`. It is saying that the project defines a complete coordinate-semantics conversion between 3DGS and UE, and every spatial or direction-dependent attribute must be interpreted under that same convention.

## 3. Render Entry Points and the Two Output Modes

The current renderer does not render directly into the Base Pass through a normal `UPrimitiveComponent` path. Instead, it inserts custom passes through `SceneViewExtension` around the post-processing stages.

There are two global modes:

- `r.GaussianSplat.RenderMode = 0`
- `r.GaussianSplat.RenderMode = 1`

The difference is described below.

### 3.1 RenderMode = 0

This mode renders during `PrePostProcessPass_RenderThread`. Gaussian splats are first accumulated into a separate intermediate texture, `GaussianAccumTexture`, and then a composite pass merges that result back into UE's `SceneColor`.

The key reason this path does not directly write every splat into `SceneColor` is that it tries to preserve the original 3DGS color accumulation semantics. The 3DGS color accumulation process first alpha-blends Gaussian colors into a final 3DGS image. The training images and target colors for 3DGS are camera-captured sRGB images, so the original 3DGS rendered result is effectively in sRGB space. UE's `SceneColor` before the tonemap pass, however, is in linear space. Therefore, the more natural path is: first accumulate the whole 3DGS image in the 3DGS style, then convert that result from sRGB into linear space, and finally feed it into UE's later rendering pipeline.

If each splat color were converted to UE linear space first and then directly blended into `SceneColor`, the order of color-space conversion and alpha blending would change. Since the $sRGB \rightarrow linear$ transform is not linear, these two orders are generally not equivalent, and the resulting color would not match exactly.

This mode is useful because:

- it stays consistent with UE HDR / PreExposure flow
- it can enter the later tonemap path
- it is better suited for integrating Gaussian splats with a UE scene

### 3.2 RenderMode = 1

This mode blends directly into `SceneColor` after tonemapping. Since color after tonemapping is already in an sRGB-like display space, the 3DGS result and the UE-rendered result can be mixed without an additional color-space conversion. The goal of this mode is to preserve the original 3DGS look as much as possible.

In this mode:

- the intermediate pre-tonemap accumulation/composite path is bypassed
- the result is closer to the original 3DGS renderer
- it is better suited for visual comparison against original 3DGS results

## 4. Why a Gaussian Can Be Drawn as a Quad

A 3D Gaussian is a continuous distribution and cannot be drawn over an infinite support region pixel by pixel. The renderer must first assign it a finite screen-space coverage region.

The current implementation follows this process:

1. Project the 3D covariance into 2D screen space, producing a 2x2 covariance matrix.
2. Decompose that 2x2 covariance matrix to obtain two principal directions and their variances.
3. Choose a finite ellipse range based on those principal axes to approximate the originally infinite 2D Gaussian.
4. Use the two finite ellipse axes to construct a correctly oriented billboard quad in screen space.
5. In the pixel shader, keep only the valid region of the ellipse and discard the rest.

The key point is that a 2D Gaussian has infinite support. It is never strictly zero across the plane. Without an artificial finite range, every splat would correspond to an infinite screen region, which obviously cannot be rasterized. The implementation therefore chooses a finite ellipse large enough to contain the meaningful contribution and truncates the far tail whose contribution is already very small.

Geometrically, eigendecomposition has a direct meaning for a 2D covariance matrix:

- eigenvectors give the two principal directions of the ellipse
- eigenvalues give the variances along those directions
- the standard deviations are $\sigma_1 = \sqrt{\lambda_1}$ and $\sigma_2 = \sqrt{\lambda_2}$

So the eigenvectors determine the orientation of the 2D Gaussian ellipse, and the square roots of the eigenvalues determine how wide it is along those axes.

The current code uses $2\sqrt{2}\sigma$ , or equivalently $\sqrt{8}\sigma$ , as the truncation range. For a standard 2D isotropic Gaussian, the probability mass inside radius $r$ satisfies:

$$
P(| x | \le r\sigma) = 1 - e^{-r^2/2}
$$

When $r = \sqrt{8}$:

$$
P = 1 - e^{-4} \approx 0.9817
$$

So this range covers about `98.2%` of the 2D Gaussian mass. The remaining tail outside this range is usually small enough to ignore for final pixel color and alpha blending. This makes $\sqrt{8}\sigma$ a reasonable quality/performance tradeoff.

The official 3DGS implementation also analyzes the 2D covariance, but it often uses a more classical `3 sigma` rule: it takes roughly three standard deviations along the principal directions to define the effective splat range. Then, instead of constructing a tightly rotated rectangle following the ellipse, the original implementation typically uses the longest radius and builds an axis-aligned square bounding box in screen space.

That approach is simple and grid-friendly, but it can cover many pixels outside the ellipse, especially when the ellipse is strongly rotated or elongated. Those extra pixels are eventually rejected or contribute very little in the pixel shader, but they still cost rasterization and pixel-shader work.

The current implementation does not use the "longest radius + axis-aligned square" bounding method. It directly uses the two principal directions of the ellipse to build a rotated rectangle in screen space. You can think of it as an approximate minimum bounding rectangle for the target ellipse. Compared with an axis-aligned square, it significantly reduces redundant coverage outside the ellipse, especially for slanted or thin ellipses.

The code also does not stop at the abstract idea of "performing eigendecomposition". It starts from the 2D covariance matrix:

$$
\Sigma_{2D} =
[[a, b], [b, d]]
$$

and first computes the eigenvalues:

$$
\lambda_{1,2} = \frac{a+d}{2} \pm \sqrt{\left(\frac{a+d}{2}\right)^2 - (ad - b^2)}
$$

Here $\lambda_1$ is the larger eigenvalue and $\lambda_2$ is the smaller one. They represent the variance along the major and minor axes of the Gaussian.

The code then uses the half-angle formula to compute the principal-axis direction angle $\theta$:

$$
\theta = \frac{1}{2}atan2(2b, a-d)
$$

It does not explicitly recover eigenvectors by plugging eigenvalues back into:

$$
(\Sigma_{2D} - \lambda I)v = 0
$$

because that approach is not always the most stable in shader code. For a 2D symmetric matrix, explicit eigenvector formulas are mathematically valid, but in numerical code they often need extra branches when $b$ is small, when the ellipse is almost axis-aligned, or when the two eigenvalues are very close.

A typical example is:

$$
\Sigma_{2D} =
[[9, 0], [0, 1]]
$$

The ellipse is already perfectly axis-aligned, and the principal directions are obviously the `x` and `y` axes. But if we mechanically write an eigenvector as:

$$
v \propto (b, \lambda - a)^T
$$

or

$$
v \propto (\lambda - d, b)^T
$$

then for the larger eigenvalue $\lambda_1 = 9$, the first form becomes:

$$
v \propto (b, \lambda_1 - a)^T = (0, 9 - 9)^T = (0, 0)^T
$$

This is a zero vector, which cannot be normalized and has no directional meaning. The other form gives:

$$
v \propto (\lambda_1 - d, b)^T = (9 - 1, 0)^T = (8, 0)^T
$$

which is the correct `x` direction. But this means the implementation must decide which branch is safe to use. In floating-point arithmetic, $b$ is often not exactly zero but a tiny value like $10^{-7}$, and the stability of the two formulas can change abruptly. Code then usually needs extra degeneracy handling such as "if $|b|$ is small enough, directly use a coordinate-axis direction".

Similarly, when $\lambda_1 \approx \lambda_2$, the ellipse is close to circular. The principal direction is not uniquely meaningful in theory, and trying to recover a specific eigenvector can make the direction jump due to tiny numerical noise. For a shader, that discontinuity is often worse than the fact that the direction is intrinsically unimportant.

By contrast, the half-angle formula describes the principal-axis direction with one continuous angle $\theta$ derived from the whole covariance matrix. It has two practical advantages:

- axis-aligned and slightly rotated ellipses can be handled by the same formula without frequent branching
- when $b \to 0$ or $\lambda_1 \approx \lambda_2$, the direction tends to behave more smoothly

Therefore, the implementation uses eigenvalues to determine the variances along the two axes, and the half-angle formula to compute the axis direction. This is better suited for GPU shader numerical behavior.

The two unit principal directions are then:

$$
e_1 = (\cos\theta, \sin\theta)^T
$$

$$
e_2 = (-\sin\theta, \cos\theta)^T
$$

Finally, the standard-deviation scale is applied to obtain the actual screen-space basis vectors used to expand the quad:

$$
b_1 = e_1 * \sqrt{8} * \sqrt{\lambda_1}
$$

$$
b_2 = e_2 * \sqrt{8} * \sqrt{\lambda_2}
$$

In the current implementation, these vectors are also multiplied by the global `splatScale` and constrained by a maximum pixel radius to prevent very large nearby splats from expanding the quad without bound.

Once $b_1$ and $b_2$ are known, the vertex shader expands a unit square in local basis space into the actual billboard sent to the rasterizer. Conceptually:

- in local parameter space, the quad starts as an axis-aligned square with range `[-1,1]^2`
- its corners are `(-1,-1)`, `(1,-1)`, `(1,1)`, and `(-1,1)`
- each corner is linearly combined along the two screen-space basis vectors $b_1$ and $b_2$

If the local corner is $c = (c_x, c_y)$, the screen-space offset is:

$$
\Delta p_{pixel} = c_x b_1 + c_y b_2
$$

This stretches and rotates the local square into a screen-space rectangle that encloses the target ellipse. The vertex shader then converts that pixel-space offset into an NDC offset by dividing by viewport width and height, adds it to the center NDC coordinate, and emits the final vertex.

The Gaussian center is first projected from view space into clip space:

$$
p_{ndc,center} = \frac{p_{clip,center}}{w_{clip,center}}
$$

The pixel-space offset $\Delta p_{pixel}$ is converted into NDC offset as:

$$
\Delta p_{ndc} =
\Delta p_{pixel}
\odot
(2/W, -2/H)^T
$$

where $W, H$ are the viewport width and height. The negative sign on the `y` component is needed because screen pixel coordinates and NDC have opposite `y` directions.

The final NDC position of each quad vertex is:

$$
p_{ndc} = p_{ndc,center} + \Delta p_{ndc}
$$

Since converting from clip space to NDC only requires the homogeneous divide, and the code has already performed that step for the center, the final output can set `w` to `1` and treat the NDC position as clip-space output:

$$
p_{clip,out} =
(p_{ndc,x}, p_{ndc,y}, p_{ndc,z}, 1)^T
$$

This is why the shader computes `ndcCenter`, adds `ndcOffset`, and outputs `float4(ndcCenter.xy + ndcOffset, ndcCenter.z, 1.0)`.

So the quad is only a rasterization bounding region. The actual splat shape is still computed as a Gaussian ellipse in the pixel shader.

## 5. What the Vertex Shader Does

The current vertex shader is `GaussianSplatVS`. Its job is not normal mesh vertex transformation. Its job is to expand one splat into six vertices forming two triangles.

The main steps are described below.

### 5.1 Read the Current Splat

In the current draw call, every `6` vertices correspond to one splat, because one quad is represented as two triangles.

The vertex shader uses:

- `SortedVisibleIndexBuffer`
- `GlobalObjectIndexBuffer`
- `PerObjectBuffer`

to determine:

- the global index of the current splat
- which Gaussian object it belongs to
- where its attributes are located in the global compressed buffers

Although these buffers are built by the sorting and upload stages, during rendering they are simply inputs to the shader.

### 5.2 Decode Current Splat Attributes

The vertex shader decodes:

- position
- color and base opacity
- rotation quaternion
- scale
- normal payload
- higher-order SH

Position is dequantized from chunk min/max data. Scale is decoded from fixed `8-bit` log-scale encoding and restored through `exp`. Rotation is decoded from a quaternion that has already been compressed under the UE basis. The current normal payload is kept for future extensions and is not used for lighting in the main rendering path.

### 5.3 Transform to View / Clip Space

Position goes through:

$$
Local \rightarrow World \rightarrow View \rightarrow Clip
$$

In code terms:

```text
MV = LocalToWorld * WorldToView
vCenter = mul(float4(splatPos, 1.0f), MV)
clipCenter = mul(vCenter, ViewToClip)
```

Here `vCenter` is the Gaussian center in view space. It is important because:

- 2D covariance projection needs it
- depth clipping needs it
- view-dependent SH color uses the corresponding view direction

### 5.4 Basic Alpha Culling

Before doing the expensive projection work, the shader checks whether the base opacity is lower than `AlphaCullThreshold`. If it is below the threshold, the splat returns invalid output and stops participating in rendering.

This is straightforward:

- very low-alpha splats contribute almost nothing to the final image
- continuing through ellipse construction and pixel evaluation would waste bandwidth and fill rate

### 5.5 View-Dependent Color

If higher-order SH is enabled, the vertex shader evaluates SH using the current view direction and adds the result to the base color.

The shader maps the UE local view direction back to the 3DGS direction convention:

$$
v_{3dgs} =
(v_{ue,y}, -v_{ue,z}, v_{ue,x})^T
$$

The reason is that SH coefficients were trained under the original 3DGS coordinate system. Evaluation must use the same direction convention, otherwise view-dependent color will be misaligned.

### 5.6 Project 3D Covariance to 2D Covariance

This is the central step of the rendering path.

The shape of a 3D Gaussian is described by a $3 \times 3$ covariance matrix. Since `rotation + scale` have already been converted into UE semantics during import and compression, the vertex shader can construct the 3D covariance directly from UE-space rotation and scale:

$$
\Sigma_{3D} = R_{ue} \, S_{ue}^{2} \, R_{ue}^{T}
$$

It is then approximately projected into screen space through the projection Jacobian:

$$
\Sigma_{2D} = J \, W \, \Sigma_{3D} \, W^{T} \, J^{T}
$$

where:

- `J` is the first-order Jacobian of the perspective projection at the current Gaussian center
- `W` is the model-view rotation part

The renderer projects the covariance matrix instead of projecting a Gaussian bounding box because the geometric essence of a Gaussian is a quadratic distribution. Projecting the covariance is what preserves the correct screen-space elliptical Gaussian shape.

This is not done by directly applying the perspective projection matrix to the covariance. Perspective projection is nonlinear, so a 3D Gaussian does not strictly remain Gaussian after perspective projection. The implementation uses a local linear approximation through the projection Jacobian at the Gaussian center. Because of this, large-FOV cases, such as fisheye-like views, can show more visible approximation error.

### 5.7 Why Low-Pass Bias Is Added

The projected 2D covariance receives an additional `0.1` on its diagonal terms:

$$
\Sigma_{2D,00} \mathrel{+}= 0.1
$$

$$
\Sigma_{2D,11} \mathrel{+}= 0.1
$$

This is a low-pass bias. It prevents splats from degenerating into numerically unstable, flickering, or highly aliased point-like results at very small projected sizes.

It can be understood as a minimum screen-space blur kernel that prevents the Gaussian from becoming too sharp after projection.

When `r.GaussianSplat.EnableAntialiasing=1`, the shader also enables the Mip-Splatting opacity compensation used by the official 3DGS antialiasing path:

$$
\alpha'=\alpha\sqrt{\frac{\det(\Sigma_{2D})}{\det(\Sigma_{2D}+0.1I)}}
$$

The low-pass filter increases the screen-space footprint. The determinant ratio reduces opacity accordingly so the splat does not become brighter just because it covers more pixels.

### 5.8 Why Eigen-Decomposition Is Needed

The 2D covariance matrix describes the shape of the ellipse, but GPU rasterization needs two screen-space basis vectors that can expand a quad.

The shader therefore computes eigenvalues and eigenvectors of the 2x2 covariance:

- eigenvectors give the principal directions of the ellipse
- eigenvalues give the scale along those principal directions

Then they are converted into two screen-space basis vectors with real length. More specifically, if the two eigenvalues are $\lambda_1, \lambda_2$ and the two unit principal directions are $e_1, e_2$ , the vertex shader needs:

$$
b_1 = e_1 * r * \sqrt{\lambda_1}
$$

$$
b_2 = e_2 * r * \sqrt{\lambda_2}
$$

where $r$ is the chosen Gaussian truncation radius. In this implementation, $r = \sqrt{8}$, so the ellipse takes $\sqrt{8}\sigma_1$ and $\sqrt{8}\sigma_2$ along the two principal directions. Since $\sqrt{\lambda_1}$ and $\sqrt{\lambda_2}$ are the standard deviations along the axes, this step simply scales unit directions to the actual ellipse semi-axis lengths.

This range is not the classical `3 sigma` range. It is `sqrt(8) sigma`, as explained earlier: it is a quality/performance tradeoff that covers enough mass without making the rasterized bounding quad too large. Compared with the original 3DGS path of `3 sigma + axis-aligned bounding box`, this path directly obtains two basis vectors that follow the ellipse orientation, which is better for constructing a compact rotated rectangle.

### 5.9 Output a Billboard Quad

Finally, the vertex shader uses six fixed local corners:

```text
(-1, -1), (1, -1), (1, 1), (-1, -1), (1, 1), (-1, 1)
```

These six corners represent a normalized square in local basis space with side length `2`. The square itself has no screen-space size or orientation yet. The real expansion maps each corner $(c_x, c_y)$ along the two screen-space basis vectors $b_1, b_2$ :

$$
\Delta p_{pixel} = c_x b_1 + c_y b_2
$$

This stretches and rotates the local square into a screen-space rectangle that encloses the target ellipse. Then the vertex shader converts the pixel-space offset into NDC by dividing by viewport width and height, adds it to the splat center NDC coordinate, and outputs it to the rasterizer.

So the vertex shader is not outputting the mathematical boundary of the Gaussian itself. It outputs a clip/NDC-compatible billboard quad that carries later pixel evaluation. The GPU rasterizer only enumerates pixels covered by this quad; the pixel shader decides which of those pixels actually belong to the effective Gaussian contribution.

This output is not the Gaussian itself. It is the screen-space drawing region that carries Gaussian pixel evaluation.

## 6. What the Pixel Shader Does

The current pixel shader is `GaussianSplatPS`. Its core goals are:

- determine whether the current pixel belongs to the valid contribution region of the splat
- compute the Gaussian weight at that pixel
- derive alpha from the weight
- output color and alpha for fixed-function `over` blending

### 6.1 Scene-Depth Rejection

If manual scene-depth testing is enabled for the current pass, the pixel shader first samples UE's scene depth texture and checks whether the current splat pixel is occluded by closer UE geometry.

If it is occluded, the pixel is clipped.

This design matters because the current Gaussian splat pass does not write its own splat depth back into UE's main depth buffer, but it still needs to respect existing UE geometry occlusion. This allows:

- Gaussian splats to be hidden behind UE meshes
- UE's scene depth to remain intact for later rendering passes

## 7. How Alpha Is Computed

This is one of the central parts of the rendering chain.

### 7.1 The 2D Gaussian Form

A screen-space Gaussian weight can be written as:

$$
w(x) = \exp\left(-\frac{1}{2}x^{T}Cx\right)
$$

where:

- $x$ is the 2D offset from the splat center to the current pixel
- $C$ is the inverse 2D covariance matrix, stored in code as `Conic`

The current implementation has two equivalent evaluation paths.

The first path is `RASTER_MODE = 1`, the conic / CUDA-like path. It directly evaluates the quadratic form in screen space. If the screen-space offset from splat center to current pixel is:

$$
x = (dx, dy)^T
$$

then the evaluation can be written as:

$$
power = -\frac{1}{2}\left(C_{xx}dx^2 + 2C_{xy}dxdy + C_{yy}dy^2\right)
$$

$$
w = \exp(power)
$$

In the shader implementation, because `Conic` stores the symmetric matrix in a compressed form, the cross term is written as:

$$
-C_{xy}dxdy
$$

This is equivalent to the matrix form above; the symmetric-term coefficient is folded into the storage convention. This path is closest to the original 3DGS / CUDA formula and is therefore the most direct to compare with the original implementation.

The second path is `RASTER_MODE = 0`. This path does not directly use the conic quadratic form in screen space. Instead, it rewrites the pixel offset into Gaussian-local parameter space. Since the billboard was expanded from a normalized square in local parameter space, the pixel shader can obtain the local Gaussian coordinate $f$ and use:

$$
A = f^{T}f
$$

$$
w = \exp\left(-\frac{1}{2}A\right)
$$

In the current code, pixels are discarded when `A > 8`, which corresponds exactly to the $\sqrt{8}\sigma$ truncation range:

$$
\|f\|^2 > 8
  \Longleftrightarrow  
\|f\| > \sqrt{8}
$$

Mathematically, `RASTER_MODE = 0` and `RASTER_MODE = 1` are equivalent. The former first transforms the pixel offset into Gaussian-local space and evaluates a standard isotropic Gaussian there. The latter folds that local Gaussian shape into the screen-space quadratic form $C$ and evaluates it directly in screen space. The expressions differ, but they represent the same 2D Gaussian distribution.

In theory, `RASTER_MODE = 0` can be slightly cheaper because it avoids explicitly expanding a screen-space quadratic form with a cross term. `RASTER_MODE = 1` is more direct and closer to the original 3DGS conic / CUDA path, making it easier to compare with the original formulas.

### 7.2 Why Some Pixels Are Discarded

If the current pixel lies outside the effective Gaussian region, it does not need to participate in blending.

The code checks cases such as:

- `power > 0`, then discard
- local parameters outside the valid range, then discard

For a valid Gaussian interior, the exponent should be non-positive. Pixels outside the effective region have no meaningful contribution and should not continue through blending.

### 7.3 Final Alpha

After computing the Gaussian weight, final alpha is:

```text
alpha = gaussianWeight * baseOpacity
```

Here `baseOpacity` is stored by the splat itself, while `gaussianWeight` decreases as the pixel moves farther from the center.

The code also performs two numerical treatments consistent with the original 3DGS path:

```text
alpha = min(0.99, ...)
if alpha <= 1/255 then discard
```

The reasons are:

- prevent a single splat from becoming a completely opaque `1.0` fragment, avoiding extreme numerical and `over`-composition cases
- discard tiny-alpha fragments whose visual contribution is negligible and whose blending cost is unnecessary

## 8. Why Alpha Blending Is Done This Way

The UE path uses standard back-to-front alpha-over blending. Farther splats are drawn first, and nearer splats are drawn over the accumulated result.

We can compare this directly with the original 3DGS formulation. Let the effective opacity of splat $i$ at the current pixel be:

$$
\alpha_i = o_i \, w_i
$$

where:

- $o_i$ is the stored base opacity of the splat
- $w_i$ is the Gaussian weight at the pixel
- $\alpha_i$ is the final effective alpha of that splat at that pixel

### 8.1 Original 3DGS Front-to-Back Recurrence

Original 3DGS is often written as a front-to-back volume-rendering recurrence. Splats are ordered from near to far, and a remaining transmittance $T$ is maintained:

$$
C \leftarrow C + T \, \alpha_i \, c_i
$$

$$
T \leftarrow T(1-\alpha_i)
$$

Here $C$ is the accumulated color and $c_i$ is the color of splat $i$. Fully expanded, the recurrence can be written as:

$$
C_{out} = \sum_{i=1}^{N}\left(\alpha_i c_i \prod_{j=1}^{i-1}(1-\alpha_j)\right)
$$

The index order here is near to far. Later splats are attenuated by the opacity accumulated from splats in front of them.

### 8.2 Current UE Back-to-Front `over` Recurrence

If the current fragment is `src` and the existing result is `dst`, the color blending rule is:

$$
c_{out} = c_{src} \alpha_{src} + c_{dst}(1-\alpha_{src})
$$

The alpha blending rule is:

$$
\alpha_{out} = \alpha_{src} + \alpha_{dst}(1-\alpha_{src})
$$

This is the classic transparent `over` rule. It works for Gaussian splats because:

- every splat is a translucent contribution in the volume-rendering approximation
- multiple splats must be layered from far to near in the final image
- each nearer layer attenuates the already accumulated contribution behind it

Expanded across many layers, the current UE path is equivalent to:

$$
C_{out} = \alpha_N c_N + (1-\alpha_N)\alpha_{N-1}c_{N-1} + \cdots + \left(\prod_{j=2}^{N}(1-\alpha_j)\right)\alpha_1 c_1
$$

If the indices are ordered from far to near, this matches the front-to-back result above.

### 8.3 Relation Between the Two and Sorting Direction

The two forms are mathematically equivalent, but they maintain different recurrence variables:

- front-to-back maintains remaining transmittance
- back-to-front maintains an already composited background color

Therefore, the main difference is not the final result, but the sorting direction.

For original 3DGS front-to-back accumulation, sorting is usually from small depth to large depth, or near to far. In the current UE back-to-front `over` path, sorting must be from large depth to small depth, or far to near. This is why the GPU sorting direction here is the opposite of the original 3DGS sorting direction, even though both serve the same goal: matching the blending recurrence with the ordering it mathematically requires.

This also explains why sorting and blending must be paired correctly. If the order is wrong, the problem is not just that the image becomes slightly worse; the mathematical meaning of alpha accumulation is broken, and the final image can become obviously incorrect.

## 9. Why Both Occlusion Cases Work

During rendering, the plugin needs to handle two different kinds of occlusion:

1. occlusion between Gaussian splats and UE meshes
2. occlusion between different Gaussian objects

These two cases are handled differently.

### 9.1 Occlusion Between Gaussian Objects and UE Meshes

Occlusion between Gaussian splats and UE meshes mainly relies on the splat pass reading UE's existing `SceneDepth`. For a splat pixel being shaded, if existing UE geometry is closer at that pixel, the splat pixel is discarded and does not participate in later color accumulation.

In other words, this kind of occlusion is decided by comparing the current Gaussian pixel against UE scene depth. This guarantees:

- if a UE mesh is in front of a Gaussian, the Gaussian is correctly clipped
- if the Gaussian is in front of the UE mesh, it can still display normally

### 9.2 Occlusion Between Different Gaussian Objects

Occlusion between Gaussian objects is not just about "having sorting". The key is that all Gaussians are merged into one unified rendering path. The current implementation does not render each Gaussian object as an independent draw call with local sorting and then concatenate objects by object order. Instead, all participating Gaussian objects are merged into the same global buffers and rendered through one merged draw call.

This has two direct benefits:

1. All splats can be globally depth-sorted, so splats from different Gaussian objects can be composited in the correct transparent order.
2. Draw call count is greatly reduced, avoiding the CPU/GPU/RHI overhead of drawing every object independently.

For Gaussian-to-Gaussian occlusion, the important point is not which object is drawn first. The important point is that all splats are merged into the same draw path and rendered according to one unified sorted result. This preserves occlusion correctness and improves rendering performance.

The details of how the global sorting buffer is generated are intentionally not expanded here, because they belong to the GPU sorting document.

## 10. Summary

The rendering part of the plugin is essentially doing this:

It takes an anisotropic Gaussian distribution defined in 3D space, applies coordinate conversion, view/clip transformation, and covariance projection, obtains a 2D Gaussian ellipse in screen space, computes per-pixel opacity in the pixel shader, and accumulates the result with standard alpha-over blending.

Therefore, the key is not "drawing a point" or "drawing a mesh". The key is:

- correctly projecting the 3D covariance
- correctly estimating the screen-space ellipse range
- correctly computing alpha
- correctly blending in the required order

As long as these four pieces are correct, 3D Gaussian splatting can render stably and continuously while remaining consistent with the volume-rendering approximation used by the original 3DGS renderer.
