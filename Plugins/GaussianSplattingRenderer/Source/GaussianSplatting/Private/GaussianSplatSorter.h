#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "RenderGraphResources.h"

class FRDGBuilder;
class FRHICommandListImmediate;

// Render-side payload produced by the GPU sorter:
//   - SortedIndexSRV stores the draw order over global splat indices
//   - DrawIndirectArgsBuffer stores FRHIDrawIndirectParameters for the merged draw call
struct FGaussianSplatSortedDrawBuffers
{
    FRDGBufferSRVRef SortedIndexSRV = nullptr;
    FRDGBufferRef DrawIndirectArgsBuffer = nullptr;
};

/**
 * GPU-only global sorter for 3D Gaussian Splatting.
 *
 * The sorter keeps only lightweight render-thread state:
 *   - the requested camera parameters for the next sort
 *   - persistent GPU buffers used by the cull + sort pipeline
 *   - the last successful sort fingerprint for lazy reuse
 *
 * Usage on the render thread each frame:
 *   1. Call RequestSort() with the current camera state.
 *   2. Call TryConsumeSorted() to execute the GPU cull + sort path (or reuse the last result).
 */
class FGaussianSplatSorter
{
public:
    FGaussianSplatSorter() = default;
    ~FGaussianSplatSorter();

    bool Initialize();
    void Shutdown();

    /**
     * Queue the next GPU sort request from the render thread.
     * When bLazy is true and the camera/config fingerprint matches the last successful sort,
     * the previous GPU result is reused and no new sort request is queued.
     */
    bool RequestSort(
        const FMatrix44f& InWorldToView,
        const FMatrix44f& InViewToClip,
        float InTanHalfFovX,
        float InTanHalfFovY,
        int32 InTotalSplats,
        bool bLazy = true);

    /**
     * Called from the render thread.
     * Executes the GPU cull + sort pipeline for the latest request, or reuses the last valid
     * GPU result if RequestSort() skipped due to bLazy.
     */
    bool TryConsumeSorted(
        FRDGBuilder& GraphBuilder,
        FRHICommandListImmediate& RHICmdList,
        const FShaderResourceViewRHIRef& GlobalPackedPositionSRV,
        const FShaderResourceViewRHIRef& GlobalPackedColorSRV,
        const FShaderResourceViewRHIRef& GlobalChunkPositionMinSRV,
        const FShaderResourceViewRHIRef& GlobalChunkPositionMaxSRV,
        const FShaderResourceViewRHIRef& GlobalObjectIndexSRV,
        const FShaderResourceViewRHIRef& PerObjectSRV,
        int32 ObjectCount,
        FGaussianSplatSortedDrawBuffers& OutBuffers);

private:
    static int32 GetCullMode();
    static float GetSplatFrustumSlack();
    static int32 GetSortConfigSignature();

    bool BuildGPUSortedDrawBuffers(
        FRDGBuilder& GraphBuilder,
        FRHICommandListImmediate& RHICmdList,
        const FShaderResourceViewRHIRef& GlobalPackedPositionSRV,
        const FShaderResourceViewRHIRef& GlobalPackedColorSRV,
        const FShaderResourceViewRHIRef& GlobalChunkPositionMinSRV,
        const FShaderResourceViewRHIRef& GlobalChunkPositionMaxSRV,
        const FShaderResourceViewRHIRef& GlobalObjectIndexSRV,
        const FShaderResourceViewRHIRef& PerObjectSRV,
        int32 ObjectCount,
        FGaussianSplatSortedDrawBuffers& OutBuffers);

    void UpdateSortFingerprint();

    void EnsureUInt32Buffer(
        FRHICommandListImmediate& RHICmdList,
        TRefCountPtr<FRDGPooledBuffer>& InOutPooled,
        FBufferRHIRef& InOutBuffer,
        FShaderResourceViewRHIRef& InOutSRV,
        FUnorderedAccessViewRHIRef& InOutUAV,
        int32& InOutCapacity,
        int32 MinElements,
        const TCHAR* Name,
        EBufferUsageFlags UsageFlags);

    void EnsureIdentityIndexBuffer(
        FRHICommandListImmediate& RHICmdList,
        int32 MinEntries);

    void EnsureDrawIndirectArgsBuffer(FRHICommandListImmediate& RHICmdList);
    void ResetGPUSortScratchBuffers();
    bool AreSortScratchBuffersValid(int32 SplatCount);

private:
    // Latest requested view state.
    bool      bHasPendingSortRequest = false;
    bool      bHasValidSortResult = false;
    FMatrix44f RequestedWorldToView = FMatrix44f::Identity;
    FMatrix44f RequestedViewToClip = FMatrix44f::Identity;
    float     RequestedTanHalfFovX = 1.0f;
    float     RequestedTanHalfFovY = 1.0f;
    int32     RequestedTotalSplats = 0;

    // Last successful sort fingerprint used by the lazy path.
    FMatrix44f LastSortedWorldToView = FMatrix44f::Identity;
    FMatrix44f LastSortedViewToClip = FMatrix44f::Identity;
    int32      LastSortedTotalSplats = -1;
    int32      LastSortConfigSignature = -1;

    // Ping-pong key buffers used by SortGPUBuffers.
    TRefCountPtr<FRDGPooledBuffer> GPUSortKeyPooled[2];
    FBufferRHIRef                  GPUSortKeyBufferRHI[2];
    FShaderResourceViewRHIRef      GPUSortKeySRV[2];
    FUnorderedAccessViewRHIRef     GPUSortKeyUAV[2];

    // Intermediate value buffers used by SortGPUBuffers while reordering global splat indices.
    TRefCountPtr<FRDGPooledBuffer> GPUSortValuePooled[2];
    FBufferRHIRef                  GPUSortValueBufferRHI[2];
    FShaderResourceViewRHIRef      GPUSortValueSRV[2];
    FUnorderedAccessViewRHIRef     GPUSortValueUAV[2];

    int32 GPUSortKeyCapacity = 0;
    int32 GPUSortValueCapacity = 0;

    // Persistent 0..N-1 identity index buffer used as the initial value stream for GPU sort.
    TRefCountPtr<FRDGPooledBuffer> IdentityIndexPooled;
    FBufferRHIRef                  IdentityIndexBufferRHI;
    FShaderResourceViewRHIRef      IdentityIndexSRV;
    int32                          IdentityIndexCapacity = 0;

    // Final draw-order buffer consumed by the vertex shader.
    TRefCountPtr<FRDGPooledBuffer> SortedIndexPooled;
    FBufferRHIRef                  SortedIndexBufferRHI;
    FShaderResourceViewRHIRef      SortedIndexSRV;
    FUnorderedAccessViewRHIRef     SortedIndexUAV;
    int32                          SortedIndexCapacity = 0;

    // Per-frame GPU cull working set.
    TRefCountPtr<FRDGPooledBuffer> ObjectVisibilityPooled;
    FBufferRHIRef                  ObjectVisibilityBufferRHI;
    FShaderResourceViewRHIRef      ObjectVisibilitySRV;
    FUnorderedAccessViewRHIRef     ObjectVisibilityUAV;
    int32                          ObjectVisibilityCapacity = 0;

    TRefCountPtr<FRDGPooledBuffer> VisibleCountPooled;
    FBufferRHIRef                  VisibleCountBufferRHI;
    FShaderResourceViewRHIRef      VisibleCountSRV;
    FUnorderedAccessViewRHIRef     VisibleCountUAV;
    int32                          VisibleCountCapacity = 0;

    // Persistent indirect draw args buffer reused by the raster pass and lazy reuse path.
    TRefCountPtr<FRDGPooledBuffer> DrawIndirectArgsPooled;
    FBufferRHIRef                  DrawIndirectArgsBufferRHI;
    FUnorderedAccessViewRHIRef     DrawIndirectArgsUAV;
};
