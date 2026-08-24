#include "GaussianSplatSorter.h"
#include "GaussianSplatCVars.h"
#include "GaussianSplatShaders.h"
#include "GaussianSplatStats.h"
#include "RHIGPUReadback.h"

#include "GPUSort.h"
#include "GPUProfiler.h"
#include "RHIBreadcrumbs.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "DeviceRadixSort.h"

DECLARE_GPU_STAT_NAMED(GaussianSplatObjectCull, TEXT("Gaussian Splat Object Cull"));
DECLARE_GPU_STAT_NAMED(GaussianSplatSortKeyGen, TEXT("Gaussian Splat Sort Key Gen"));
DECLARE_GPU_STAT_NAMED(GaussianSplatGPUSort, TEXT("Gaussian Splat GPU Sort (UE Built-in)"));
DECLARE_GPU_STAT_NAMED(GaussianSplatDeviceRadixSort, TEXT("Gaussian Splat GPU Sort (DeviceRadix)"));
DECLARE_GPU_STAT_NAMED(GaussianSplatIndirectArgs, TEXT("Gaussian Splat Indirect Args"));

namespace
{
static constexpr int32 GGaussianSplatBuildSortKeysGroupSize = 64;

static int32 GetMaxDispatchGroupsX()
{
    return FMath::Max(1, int32(GRHIGlobals.MaxDispatchThreadGroupsPerDimension.X));
}
}

// ============================================================
// Lifetime
// ============================================================

FGaussianSplatSorter::~FGaussianSplatSorter()
{
    Shutdown();
}

bool FGaussianSplatSorter::Initialize()
{
    return true;
}

void FGaussianSplatSorter::Shutdown()
{
    bHasPendingSortRequest = false;
    bHasValidSortResult = false;

    SortedIndexPooled.SafeRelease();
    SortedIndexBufferRHI.SafeRelease();
    SortedIndexSRV.SafeRelease();
    SortedIndexUAV.SafeRelease();
    SortedIndexCapacity = 0;

    ResetGPUSortScratchBuffers();

    IdentityIndexPooled.SafeRelease();
    IdentityIndexBufferRHI.SafeRelease();
    IdentityIndexSRV.SafeRelease();
    IdentityIndexCapacity = 0;

    ObjectVisibilityPooled.SafeRelease();
    ObjectVisibilityBufferRHI.SafeRelease();
    ObjectVisibilitySRV.SafeRelease();
    ObjectVisibilityUAV.SafeRelease();
    ObjectVisibilityCapacity = 0;

    VisibleCountPooled.SafeRelease();
    VisibleCountBufferRHI.SafeRelease();
    VisibleCountSRV.SafeRelease();
    VisibleCountUAV.SafeRelease();
    VisibleCountCapacity = 0;

    DrawIndirectArgsPooled.SafeRelease();
    DrawIndirectArgsBufferRHI.SafeRelease();
    DrawIndirectArgsUAV.SafeRelease();

    if (VisibleCountReadback)
    {
        delete VisibleCountReadback;
        VisibleCountReadback = nullptr;
    }
    bVisibleCountReadbackPending = false;
}

// ============================================================
// RequestSort
//
// Called from the render thread.
//
// This function only records the view-dependent sort request for the current frame.
// The actual GPU cull + key generation + radix sort work is deferred until
// TryConsumeSorted(), because that later stage has access to the per-frame GPU inputs
// (PerObjectBuffer SRV and the merged static buffer SRVs) required by the compute passes.
//
// When bLazy is enabled and the view/config fingerprint matches the last successful sort,
// this function skips queuing a new request so the previous GPU result can be reused.
// ============================================================
bool FGaussianSplatSorter::RequestSort(
    const FMatrix44f& InWorldToView,
    const FMatrix44f& InViewToClip,
    float InTanHalfFovX,
    float InTanHalfFovY,
    float InMaxFocalLengthPixels,
    int32 InTotalSplats,
    bool bLazy)
{
    const int32 NewSortConfigSignature = GetSortConfigSignature();

    // Empty scenes do not need a GPU sort request. Clear the pending/valid state so the
    // caller will naturally skip the draw path for this frame.
    if (InTotalSplats <= 0)
    {
        bHasPendingSortRequest = false;
        bHasValidSortResult = false;
        RequestedTotalSplats = 0;
        return false;
    }

    // If sort-relevant runtime settings changed, the previous GPU
    // result can no longer be reused safely.
    if (LastSortConfigSignature != NewSortConfigSignature)
    {
        bHasValidSortResult = false;
    }

    // Lazy reuse:
    // If the camera transform, projection and total splat count all match the last
    // successful GPU sort closely enough, skip rebuilding the sort request and keep
    // consuming the previous GPU result.
    if (bLazy && bHasValidSortResult
        && LastSortConfigSignature == NewSortConfigSignature
        && LastSortedTotalSplats == InTotalSplats
        && FMath::Abs(LastSortedMaxFocalLengthPixels - InMaxFocalLengthPixels) <= 1e-3f)
    {
        bool bSame = true;
        for (int32 Row = 0; Row < 4 && bSame; ++Row)
        {
            for (int32 Col = 0; Col < 4 && bSame; ++Col)
            {
                if (FMath::Abs(InWorldToView.M[Row][Col] - LastSortedWorldToView.M[Row][Col]) > 1e-3f
                    || FMath::Abs(InViewToClip.M[Row][Col] - LastSortedViewToClip.M[Row][Col]) > 1e-3f)
                {
                    bSame = false;
                }
            }
        }

        if (bSame)
        {
            bHasPendingSortRequest = false;
            return false;
        }
    }

    // Record the latest camera/projection request. TryConsumeSorted() will later read this
    // state and execute the GPU cull + sort passes against the current frame's GPU buffers.
    RequestedWorldToView = InWorldToView;
    RequestedViewToClip = InViewToClip;
    RequestedTanHalfFovX = InTanHalfFovX;
    RequestedTanHalfFovY = InTanHalfFovY;
    RequestedMaxFocalLengthPixels = InMaxFocalLengthPixels;
    RequestedTotalSplats = InTotalSplats;
    bHasPendingSortRequest = true;
    return true;
}

// ============================================================
// TryConsumeSorted
//
// Called from the render thread.
//
// This is the point where the GPU sort request is materialized into real draw inputs.
// If RequestSort() queued a new request, this function executes the GPU object cull,
// per-splat cull + sort-key generation, GPU radix sort and indirect-args build passes.
//
// If RequestSort() skipped due to the lazy path, this function simply reuses the last
// valid GPU result buffers and exposes them again to the current frame through RDG.
// ============================================================
bool FGaussianSplatSorter::TryConsumeSorted(
    FRDGBuilder& GraphBuilder,
    FRHICommandListImmediate& RHICmdList,
    const FShaderResourceViewRHIRef& GlobalPackedPositionSRV,
    const FShaderResourceViewRHIRef& GlobalPackedColorSRV,
    const FShaderResourceViewRHIRef& GlobalPackedScaleSRV,
    const FShaderResourceViewRHIRef& GlobalChunkPositionMinSRV,
    const FShaderResourceViewRHIRef& GlobalChunkPositionMaxSRV,
    const FShaderResourceViewRHIRef& GlobalObjectIndexSRV,
    const FShaderResourceViewRHIRef& PerObjectSRV,
    int32 ObjectCount,
    FGaussianSplatSortedDrawBuffers& OutBuffers)
{
    OutBuffers = {};

    // ---- Stat updates (visible via `stat GaussianSplat`) ----
    // "Before Cull" is CPU-known from the queued request and is refreshed every frame.
    // (In the lazy/reuse path RequestedTotalSplats still holds the current scene's splat count,
    // since a matching view fingerprint implies an unchanged splat set.)
    SET_DWORD_STAT(STAT_GaussianSplat_SplatsBeforeCull, static_cast<uint32>(FMath::Max<int32>(0, RequestedTotalSplats)));

    // "After Cull" (visible splats) is a GPU value written by the per-splat cull pass into
    // VisibleCountBufferRHI. It was copied into an async readback during the previous sort; read
    // it back now. Lock() blocks until the GPU fence is signaled, so the stat always gets a valid
    // value (lagging ~1 frame behind the current view).
    if (VisibleCountReadback && bVisibleCountReadbackPending)
    {
        if (void* Data = VisibleCountReadback->Lock(sizeof(uint32)))
        {
            const uint32 VisibleSplats = *static_cast<uint32*>(Data);
            SET_DWORD_STAT(STAT_GaussianSplat_SplatsAfterCull, VisibleSplats);
        }
        VisibleCountReadback->Unlock();
        bVisibleCountReadbackPending = false;
    }

    // A new request was queued for this frame, so execute the GPU cull + sort pipeline now
    // against the current frame's merged static buffers and per-object descriptor buffer.
    if (bHasPendingSortRequest)
    {
        if (!BuildGPUSortedDrawBuffers(
            GraphBuilder,
            RHICmdList,
            GlobalPackedPositionSRV,
            GlobalPackedColorSRV,
            GlobalPackedScaleSRV,
            GlobalChunkPositionMinSRV,
            GlobalChunkPositionMaxSRV,
            GlobalObjectIndexSRV,
            PerObjectSRV,
            ObjectCount,
            OutBuffers))
        {
            return false;
        }

        // The freshly built buffers are now the current reusable GPU sort result.
        bHasPendingSortRequest = false;
        bHasValidSortResult = true;
        UpdateSortFingerprint();
        return true;
    }

    // No new request was queued. Reuse the last valid GPU result if it still exists.
    if (!bHasValidSortResult || !SortedIndexPooled.IsValid() || !VisibleCountPooled.IsValid()
        || !DrawIndirectArgsPooled.IsValid())
    {
        return false;
    }

    // Re-register the persistent result buffers into the current frame's RDG graph so the
    // raster pass can consume them again without rebuilding the GPU sort.
    FRDGBufferRef SortedIndexBuffer = GraphBuilder.RegisterExternalBuffer(SortedIndexPooled, TEXT("GS_SortedGlobalSplatIndices"));
    FRDGBufferRef VisibleCountBuffer = GraphBuilder.RegisterExternalBuffer(VisibleCountPooled, TEXT("GS_VisibleSplatCount"));
    FRDGBufferRef DrawIndirectArgsBuffer = GraphBuilder.RegisterExternalBuffer(DrawIndirectArgsPooled, TEXT("GS_SplatDrawIndirectArgs"));
    OutBuffers.SortedIndexSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SortedIndexBuffer, PF_R32_UINT));
    OutBuffers.VisibleCountSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VisibleCountBuffer, PF_R32_UINT));
    OutBuffers.DrawIndirectArgsBuffer = DrawIndirectArgsBuffer;
    return true;
}

// ============================================================
// CVar helpers
// ============================================================

int32 FGaussianSplatSorter::GetCullMode()
{
    return GaussianSplatCVars::GetCullModeOnAnyThread();
}

float FGaussianSplatSorter::GetSplatFrustumSlack()
{
    return GaussianSplatCVars::GetSplatFrustumSlackOnAnyThread();
}

int32 FGaussianSplatSorter::GetSortConfigSignature()
{
    uint32 Signature = GetTypeHash(GetCullMode());
    Signature = HashCombine(Signature, GetTypeHash(GaussianSplatCVars::GetScreenSizeCullOnAnyThread()));
    Signature = HashCombine(Signature, GetTypeHash(GaussianSplatCVars::GetScreenSizeCullMinPixelsOnAnyThread()));
    Signature = HashCombine(Signature, GetTypeHash(GaussianSplatCVars::GetSortMethodOnRenderThread()));
    Signature = HashCombine(Signature, GetTypeHash(GaussianSplatCVars::GetDeviceRadixPassCountOnRenderThread()));
    Signature = HashCombine(Signature, GetTypeHash(GaussianSplatCVars::GetDeviceRadixWriteFinalKeysOnRenderThread()));
    return static_cast<int32>(Signature);
}

void FGaussianSplatSorter::UpdateSortFingerprint()
{
    LastSortedWorldToView = RequestedWorldToView;
    LastSortedViewToClip = RequestedViewToClip;
    LastSortedMaxFocalLengthPixels = RequestedMaxFocalLengthPixels;
    LastSortedTotalSplats = RequestedTotalSplats;
    LastSortConfigSignature = GetSortConfigSignature();
}

// ============================================================
// BuildGPUSortedDrawBuffers
//
// Executes the full GPU-side cull + sort pipeline for the current request:
//   1. Ensure all persistent GPU working buffers exist
//   2. Run object-level cull over the per-object descriptor buffer
//   3. Run per-splat cull + sort-key generation over the merged global splat buffers
//   4. Run UE's GPU radix sort over the global splat index stream
//   5. Build indirect draw arguments from the GPU-visible splat count
//   6. Register the final sorted-index and indirect-args buffers into the current RDG graph
//
// The sort value stream is the identity global-splat index [0..N-1], so the final
// permutation directly indexes the merged global splat buffers used by the vertex shader.
// ============================================================
bool FGaussianSplatSorter::BuildGPUSortedDrawBuffers(
    FRDGBuilder& GraphBuilder,
    FRHICommandListImmediate& RHICmdList,
    const FShaderResourceViewRHIRef& GlobalPackedPositionSRV,
    const FShaderResourceViewRHIRef& GlobalPackedColorSRV,
    const FShaderResourceViewRHIRef& GlobalPackedScaleSRV,
    const FShaderResourceViewRHIRef& GlobalChunkPositionMinSRV,
    const FShaderResourceViewRHIRef& GlobalChunkPositionMaxSRV,
    const FShaderResourceViewRHIRef& GlobalObjectIndexSRV,
    const FShaderResourceViewRHIRef& PerObjectSRV,
    int32 ObjectCount,
    FGaussianSplatSortedDrawBuffers& OutBuffers)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_BuildGPUSortedDrawBuffers_RT);
    OutBuffers = {};

    const int32 TotalSplatCount = RequestedTotalSplats;
    // All GPU passes in this function operate on the merged global splat stream, so the
    // total splat count and the SRVs for the merged buffers must already be valid.
    if (TotalSplatCount <= 0 || !GlobalPackedPositionSRV.IsValid() || !GlobalPackedColorSRV.IsValid()
        || !GlobalPackedScaleSRV.IsValid()
        || !GlobalChunkPositionMinSRV.IsValid() || !GlobalChunkPositionMaxSRV.IsValid()
        || !GlobalObjectIndexSRV.IsValid() || !PerObjectSRV.IsValid() || ObjectCount <= 0)
    {
        return false;
    }

    if (!AreSortScratchBuffersValid(TotalSplatCount))
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_ResetSortScratchBuffers_RT);
        ResetGPUSortScratchBuffers();
    }

    // Ensure the final output buffer and all compute working buffers are large enough for
    // the current object/splat counts before dispatching any GPU passes.
    const EBufferUsageFlags SortBufferUsage =
        EBufferUsageFlags::Static |
        EBufferUsageFlags::ShaderResource |
        EBufferUsageFlags::UnorderedAccess;

    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_EnsureSortedIndexBuffer_RT);
        EnsureUInt32Buffer(
            RHICmdList,
            SortedIndexPooled,
            SortedIndexBufferRHI,
            SortedIndexSRV,
            SortedIndexUAV,
            SortedIndexCapacity,
            TotalSplatCount,
            TEXT("GS_SortedGlobalSplatIndices"),
            SortBufferUsage);
    }

    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_EnsureSortKeyBuffers_RT);
        EnsureUInt32Buffer(
            RHICmdList,
            GPUSortKeyPooled[0],
            GPUSortKeyBufferRHI[0],
            GPUSortKeySRV[0],
            GPUSortKeyUAV[0],
            GPUSortKeyCapacity,
            TotalSplatCount,
            TEXT("GS_SortKeys0"),
            SortBufferUsage);
        EnsureUInt32Buffer(
            RHICmdList,
            GPUSortKeyPooled[1],
            GPUSortKeyBufferRHI[1],
            GPUSortKeySRV[1],
            GPUSortKeyUAV[1],
            GPUSortKeyCapacity,
            TotalSplatCount,
            TEXT("GS_SortKeys1"),
            SortBufferUsage);
    }

    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_EnsureSortValueBuffers_RT);
        EnsureUInt32Buffer(
            RHICmdList,
            GPUSortValuePooled[0],
            GPUSortValueBufferRHI[0],
            GPUSortValueSRV[0],
            GPUSortValueUAV[0],
            GPUSortValueCapacity,
            TotalSplatCount,
            TEXT("GS_SortValues0"),
            SortBufferUsage);
        EnsureUInt32Buffer(
            RHICmdList,
            GPUSortValuePooled[1],
            GPUSortValueBufferRHI[1],
            GPUSortValueSRV[1],
            GPUSortValueUAV[1],
            GPUSortValueCapacity,
            TotalSplatCount,
            TEXT("GS_SortValues1"),
            SortBufferUsage);
    }

    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_EnsureIdentityIndexBuffer_RT);
        EnsureIdentityIndexBuffer(RHICmdList, TotalSplatCount);
    }

    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_EnsureObjectVisibilityBuffer_RT);
        EnsureUInt32Buffer(
            RHICmdList,
            ObjectVisibilityPooled,
            ObjectVisibilityBufferRHI,
            ObjectVisibilitySRV,
            ObjectVisibilityUAV,
            ObjectVisibilityCapacity,
            ObjectCount,
            TEXT("GS_ObjectVisibility"),
            SortBufferUsage);
    }

    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_EnsureVisibleCountBuffer_RT);
        EnsureUInt32Buffer(
            RHICmdList,
            VisibleCountPooled,
            VisibleCountBufferRHI,
            VisibleCountSRV,
            VisibleCountUAV,
            VisibleCountCapacity,
            1,
            TEXT("GS_VisibleCount"),
            SortBufferUsage);
    }

    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_EnsureDrawIndirectArgsBuffer_RT);
        EnsureDrawIndirectArgsBuffer(RHICmdList);
    }

    // Reset the visible-splat counter before the GPU cull/keygen pass accumulates into it.
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_ResetVisibleCount_RT);
        uint32* VisibleCountDst = static_cast<uint32*>(RHICmdList.LockBuffer(
            VisibleCountBufferRHI,
            0,
            sizeof(uint32),
            RLM_WriteOnly));
        VisibleCountDst[0] = 0u;
        RHICmdList.UnlockBuffer(VisibleCountBufferRHI);
    }

    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
    if (!ShaderMap)
    {
        return false;
    }

    // 1. Object-level cull.
    //    Each object writes one visibility bit based on a conservative 6-plane AABB
    //    frustum test. Per-splat cull later reuses this buffer as a coarse early-out.
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_ObjectCullCS_RT);
        RHI_BREADCRUMB_EVENT_STAT(RHICmdList, GaussianSplatObjectCull, "GaussianSplat_ObjectCull");
        TShaderMapRef<FGaussianObjectCullCS> ComputeShader(ShaderMap);
        FGaussianObjectCullCS::FParameters Parameters;
        Parameters.PerObjectBuffer = PerObjectSRV;
        Parameters.ObjectCount = ObjectCount;
        Parameters.CullMode = GetCullMode();
        Parameters.WorldToView = RequestedWorldToView;
        Parameters.ViewToClip = RequestedViewToClip;
        Parameters.OutObjectVisibility = ObjectVisibilityUAV;

        RHICmdList.Transition(FRHITransitionInfo(ObjectVisibilityUAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
        FComputeShaderUtils::Dispatch(
            RHICmdList,
            ComputeShader,
            Parameters,
            FIntVector(FMath::DivideAndRoundUp(ObjectCount, 64), 1, 1));
        RHICmdList.Transition(FRHITransitionInfo(ObjectVisibilityUAV, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
    }

    // 2. Per-splat cull + depth-key generation.
    //    This walks the full merged global splat stream. Visible splats contribute to the
    //    visible counter and receive a sortable negative-depth key; culled splats receive
    //    a maximal key so they sort to the tail and are skipped by indirect draw.
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_BuildSortKeysCS_RT);
        RHI_BREADCRUMB_EVENT_STAT(RHICmdList, GaussianSplatSortKeyGen, "GaussianSplat_SortKeyGen");
        TShaderMapRef<FGaussianBuildSortKeysCS> ComputeShader(ShaderMap);
        FGaussianBuildSortKeysCS::FParameters Parameters;
        Parameters.GlobalPackedPositionBuffer = GlobalPackedPositionSRV;
        Parameters.GlobalPackedColorBuffer = GlobalPackedColorSRV;
        Parameters.GlobalPackedScaleBuffer = GlobalPackedScaleSRV;
        Parameters.GlobalChunkPositionMinBuffer = GlobalChunkPositionMinSRV;
        Parameters.GlobalChunkPositionMaxBuffer = GlobalChunkPositionMaxSRV;
        Parameters.PerObjectBuffer = PerObjectSRV;
        Parameters.GlobalObjectIndexBuffer = GlobalObjectIndexSRV;
        Parameters.ObjectVisibilityBuffer = ObjectVisibilitySRV;
        Parameters.TotalSplatCount = TotalSplatCount;
        Parameters.SplatDispatchOffset = 0u;
        Parameters.CullMode = GetCullMode();
        Parameters.TanHalfFovX = RequestedTanHalfFovX;
        Parameters.TanHalfFovY = RequestedTanHalfFovY;
        Parameters.FrustumSlack = GetSplatFrustumSlack();
        Parameters.EnableScreenSizeCull = GaussianSplatCVars::GetScreenSizeCullOnAnyThread() != 0 ? 1u : 0u;
        Parameters.ScreenSizeCullMinPixels = GaussianSplatCVars::GetScreenSizeCullMinPixelsOnAnyThread();
        Parameters.MaxFocalLengthPixels = RequestedMaxFocalLengthPixels;
        Parameters.WorldToView = RequestedWorldToView;
        Parameters.OutDepthKeys = GPUSortKeyUAV[0];
        Parameters.OutVisibleCount = VisibleCountUAV;

        // The key-generation shader writes both the sortable depth-key stream and the
        // visible splat counter, so both resources must be in UAV state for this pass.
        RHICmdList.Transition(FRHITransitionInfo(GPUSortKeyUAV[0], ERHIAccess::Unknown, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleCountUAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

        // Large scenes can exceed the maximum X dispatch-group count supported by the RHI.
        // Split the global splat stream into several dispatches; SplatDispatchOffset lets
        // the shader map each local thread back to the correct global splat index.
        const int32 MaxGroupsPerDispatch = GetMaxDispatchGroupsX();
        for (int32 DispatchOffset = 0; DispatchOffset < TotalSplatCount;)
        {
            const int32 RemainingSplats = TotalSplatCount - DispatchOffset;
            const int32 RemainingGroups = FMath::DivideAndRoundUp(RemainingSplats, GGaussianSplatBuildSortKeysGroupSize);
            const int32 GroupCount = FMath::Min(RemainingGroups, MaxGroupsPerDispatch);
            Parameters.SplatDispatchOffset = uint32(DispatchOffset);

            FComputeShaderUtils::Dispatch(
                RHICmdList,
                ComputeShader,
                Parameters,
                FIntVector(GroupCount, 1, 1));

            DispatchOffset += GroupCount * GGaussianSplatBuildSortKeysGroupSize;
        }

        // The following GPU sort and indirect-args passes read the generated keys/counter.
        RHICmdList.Transition(FRHITransitionInfo(GPUSortKeyUAV[0], ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleCountUAV, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
    }

    // 3. GPU radix sort over the full global splat stream.
    //    Keys come from the previous pass, and values are the identity global-splat index
    //    stream. The final output is therefore a sorted permutation of global splat indices.
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_SortGPUBuffers_RT);
        bool bUseDeviceRadixSort =
            GaussianSplatCVars::GetSortMethodOnRenderThread() == 1;
        if (bUseDeviceRadixSort && !DeviceRadixSort::IsSupported())
        {
            UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: DeviceRadixSort requires wave operations on this RHI; falling back to SortGPUBuffers."));
            bUseDeviceRadixSort = false;
        }

        if (bUseDeviceRadixSort)
        {
            // ---- Tuned DeviceRadixSort path ----
            // Configurable 8-bit LSD passes with identity-payload generation and an optional
            // values-only final pass to reduce mobile memory traffic.
            RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplatDeviceRadixSort, "GaussianSplat_DeviceRadixSort");
            {
                FRDGBufferRef RDGKeys[2] = {
                    GraphBuilder.RegisterExternalBuffer(GPUSortKeyPooled[0], TEXT("GS_DeviceRadixKeys0")),
                    GraphBuilder.RegisterExternalBuffer(GPUSortKeyPooled[1], TEXT("GS_DeviceRadixKeys1"))
                };
                FRDGBufferRef RDGValues[2] = {
                    GraphBuilder.RegisterExternalBuffer(GPUSortValuePooled[0], TEXT("GS_DeviceRadixValues0")),
                    GraphBuilder.RegisterExternalBuffer(GPUSortValuePooled[1], TEXT("GS_DeviceRadixValues1"))
                };
                FRDGBufferRef RDGSortedIndex = GraphBuilder.RegisterExternalBuffer(SortedIndexPooled, TEXT("GS_DeviceRadixSortedIndex"));

                const uint32 PassCount = static_cast<uint32>(GaussianSplatCVars::GetDeviceRadixPassCountOnRenderThread());
                DeviceRadixSort::FOptions SortOptions;
                SortOptions.PassCount = PassCount;
                SortOptions.FirstBit = 32u - PassCount * 8u;
                SortOptions.bWriteFinalKeys = GaussianSplatCVars::GetDeviceRadixWriteFinalKeysOnRenderThread() != 0;
                SortOptions.bFirstPayloadIsIdentity = true;

                DeviceRadixSort::Enqueue(
                    GraphBuilder,
                    RDGKeys,
                    RDGValues,
                    static_cast<uint32>(TotalSplatCount),
                    RDGSortedIndex,
                    nullptr,
                    &SortOptions);

                // The final downsweep writes values directly to the persistent output buffer.
            }
        }
        else
        {
            // ---- UE built-in SortGPUBuffers path ----
            // This path issues immediate RHI work, so the timing scope has to live on the RHI
            // timeline as a breadcrumb carrying the stat.
            RHI_BREADCRUMB_EVENT_STAT(RHICmdList, GaussianSplatGPUSort, "GaussianSplat_UEBuiltinSort");

            FGPUSortBuffers SortBuffers;
            for (int32 BufferIndex = 0; BufferIndex < 2; ++BufferIndex)
            {
                SortBuffers.RemoteKeySRVs[BufferIndex] = GPUSortKeySRV[BufferIndex];
                SortBuffers.RemoteKeyUAVs[BufferIndex] = GPUSortKeyUAV[BufferIndex];
                SortBuffers.RemoteValueSRVs[BufferIndex] = GPUSortValueSRV[BufferIndex];
                SortBuffers.RemoteValueUAVs[BufferIndex] = GPUSortValueUAV[BufferIndex];
            }
            SortBuffers.FirstValuesSRV = IdentityIndexSRV;
            SortBuffers.FinalValuesUAV = SortedIndexUAV;

            RHICmdList.Transition(FRHITransitionInfo(SortBuffers.FinalValuesUAV, ERHIAccess::Unknown, ERHIAccess::SRVCompute));
            SortGPUBuffers(
                RHICmdList,
                SortBuffers,
                0,
                0xFFFFFFFFu,
                TotalSplatCount,
                GMaxRHIFeatureLevel);
            RHICmdList.Transition(FRHITransitionInfo(SortBuffers.FinalValuesUAV, ERHIAccess::Unknown, ERHIAccess::SRVMask));
        }
    }

    // 4. Convert the GPU visible counter into both VS draw args and mesh-dispatch args.
    //    Both paths consume the same visible prefix of the sorted global permutation.
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_BuildIndirectArgsCS_RT);
        RHI_BREADCRUMB_EVENT_STAT(RHICmdList, GaussianSplatIndirectArgs, "GaussianSplat_IndirectArgs");
        TShaderMapRef<FGaussianBuildIndirectArgsCS> ComputeShader(ShaderMap);
        FGaussianBuildIndirectArgsCS::FParameters Parameters;
        Parameters.VisibleCountBuffer = VisibleCountSRV;
        Parameters.OutDrawIndirectArgs = DrawIndirectArgsUAV;

        RHICmdList.Transition(FRHITransitionInfo(DrawIndirectArgsUAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
        FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, Parameters, FIntVector(1, 1, 1));
        RHICmdList.Transition(FRHITransitionInfo(DrawIndirectArgsUAV, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
    }

    // 5. Re-expose the persistent result buffers as RDG resources for the current frame.
    FRDGBufferRef SortedIndexBuffer = GraphBuilder.RegisterExternalBuffer(SortedIndexPooled, TEXT("GS_SortedGlobalSplatIndices"));
    FRDGBufferRef VisibleCountBuffer = GraphBuilder.RegisterExternalBuffer(VisibleCountPooled, TEXT("GS_VisibleSplatCount"));
    FRDGBufferRef DrawIndirectArgsBuffer = GraphBuilder.RegisterExternalBuffer(DrawIndirectArgsPooled, TEXT("GS_SplatDrawIndirectArgs"));
    OutBuffers.SortedIndexSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SortedIndexBuffer, PF_R32_UINT));
    OutBuffers.VisibleCountSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VisibleCountBuffer, PF_R32_UINT));
    OutBuffers.DrawIndirectArgsBuffer = DrawIndirectArgsBuffer;

    // Enqueue a non-blocking GPU readback of the visible-splat counter so that
    // STAT_GaussianSplat_SplatsAfterCull can be populated on a subsequent frame (see
    // TryConsumeSorted). VisibleCountBufferRHI now holds the count written by the
    // per-splat cull pass; only re-arm the readback once the previous one was consumed.
    if (VisibleCountBufferRHI.IsValid() && !bVisibleCountReadbackPending)
    {
        if (!VisibleCountReadback)
        {
            VisibleCountReadback = new FRHIGPUBufferReadback(TEXT("GS_VisibleCountReadback"));
        }
        // VisibleCountUAV is in SRVCompute state (left by the BuildIndirectArgsCS pass).
        // CopyToStagingBuffer needs the source in CopySrc state, so transition first.
        RHICmdList.Transition(FRHITransitionInfo(VisibleCountUAV, ERHIAccess::SRVCompute, ERHIAccess::CopySrc));
        VisibleCountReadback->EnqueueCopy(RHICmdList, VisibleCountBufferRHI, sizeof(uint32));
        // Restore to SRVCompute so the next frame's transition (Unknown->UAVCompute) works.
        RHICmdList.Transition(FRHITransitionInfo(VisibleCountUAV, ERHIAccess::CopySrc, ERHIAccess::SRVCompute));
        bVisibleCountReadbackPending = true;
    }

    return true;
}

// ============================================================
// Buffer helpers
// ============================================================

void FGaussianSplatSorter::EnsureUInt32Buffer(
    FRHICommandListImmediate& RHICmdList,
    TRefCountPtr<FRDGPooledBuffer>& InOutPooled,
    FBufferRHIRef& InOutBuffer,
    FShaderResourceViewRHIRef& InOutSRV,
    FUnorderedAccessViewRHIRef& InOutUAV,
    int32& InOutCapacity,
    int32 MinElements,
    const TCHAR* Name,
    EBufferUsageFlags UsageFlags)
{
    if (InOutCapacity >= MinElements
        && InOutPooled.IsValid()
        && InOutBuffer.IsValid()
        && InOutSRV.IsValid()
        && InOutUAV.IsValid())
    {
        return;
    }

    const int32 NewCapacity = FMath::Max(MinElements + MinElements / 4 + 16, 16);
    const uint32 BufferSize = uint32(NewCapacity * sizeof(uint32));
    FRHIResourceCreateInfo CreateInfo(Name);

    InOutBuffer = RHICmdList.CreateVertexBuffer(BufferSize, UsageFlags, CreateInfo);
    InOutSRV = RHICmdList.CreateShaderResourceView(InOutBuffer, sizeof(uint32), PF_R32_UINT);
    InOutUAV = RHICmdList.CreateUnorderedAccessView(InOutBuffer, PF_R32_UINT);

    FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NewCapacity);
    Desc.Usage = UsageFlags;
    InOutPooled = new FRDGPooledBuffer(TRefCountPtr<FRHIBuffer>(InOutBuffer), Desc, NewCapacity, Name);
    InOutCapacity = NewCapacity;
}

void FGaussianSplatSorter::EnsureIdentityIndexBuffer(
    FRHICommandListImmediate& RHICmdList,
    int32 MinEntries)
{
    if (IdentityIndexCapacity >= MinEntries && IdentityIndexPooled.IsValid() && IdentityIndexBufferRHI.IsValid())
    {
        return;
    }

    const int32 NewCapacity = FMath::Max(MinEntries + MinEntries / 4 + 16, 16);
    const uint32 BufferSize = uint32(NewCapacity * sizeof(uint32));
    FRHIResourceCreateInfo CreateInfo(TEXT("GS_IdentityIndices"));
    const EBufferUsageFlags UsageFlags =
        EBufferUsageFlags::Static |
        EBufferUsageFlags::ShaderResource;

    IdentityIndexBufferRHI = RHICmdList.CreateVertexBuffer(BufferSize, UsageFlags, CreateInfo);
    IdentityIndexSRV = RHICmdList.CreateShaderResourceView(IdentityIndexBufferRHI, sizeof(uint32), PF_R32_UINT);

    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_InitIdentityIndexBuffer_RT);
        uint32* Dst = static_cast<uint32*>(RHICmdList.LockBuffer(
            IdentityIndexBufferRHI,
            0,
            BufferSize,
            RLM_WriteOnly));
        for (int32 Index = 0; Index < NewCapacity; ++Index)
        {
            Dst[Index] = uint32(Index);
        }
        RHICmdList.UnlockBuffer(IdentityIndexBufferRHI);
    }

    FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NewCapacity);
    Desc.Usage = UsageFlags;
    IdentityIndexPooled = new FRDGPooledBuffer(TRefCountPtr<FRHIBuffer>(IdentityIndexBufferRHI), Desc, NewCapacity, TEXT("GS_IdentityIndices"));
    IdentityIndexCapacity = NewCapacity;
}

void FGaussianSplatSorter::EnsureDrawIndirectArgsBuffer(FRHICommandListImmediate& RHICmdList)
{
    if (DrawIndirectArgsPooled.IsValid() && DrawIndirectArgsBufferRHI.IsValid() && DrawIndirectArgsUAV.IsValid())
    {
        return;
    }

    static constexpr uint32 IndirectRecordCount = 2u;
    const uint32 BufferSize = sizeof(FRHIDrawIndirectParameters) * IndirectRecordCount;
    FRHIResourceCreateInfo CreateInfo(TEXT("GS_SplatDrawIndirectArgs"));
    const EBufferUsageFlags UsageFlags =
        EBufferUsageFlags::DrawIndirect |
        EBufferUsageFlags::UnorderedAccess |
        EBufferUsageFlags::ShaderResource;

    DrawIndirectArgsBufferRHI = RHICmdList.CreateVertexBuffer(BufferSize, UsageFlags, CreateInfo);
    DrawIndirectArgsUAV = RHICmdList.CreateUnorderedAccessView(DrawIndirectArgsBufferRHI, PF_R32_UINT);

    FRDGBufferDesc Desc = FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndirectParameters>(IndirectRecordCount);
    Desc.Usage |= EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
    DrawIndirectArgsPooled = new FRDGPooledBuffer(TRefCountPtr<FRHIBuffer>(DrawIndirectArgsBufferRHI), Desc, IndirectRecordCount, TEXT("GS_SplatDrawIndirectArgs"));
}

void FGaussianSplatSorter::ResetGPUSortScratchBuffers()
{
    for (int32 BufferIndex = 0; BufferIndex < 2; ++BufferIndex)
    {
        GPUSortKeyPooled[BufferIndex].SafeRelease();
        GPUSortKeyBufferRHI[BufferIndex].SafeRelease();
        GPUSortKeySRV[BufferIndex].SafeRelease();
        GPUSortKeyUAV[BufferIndex].SafeRelease();

        GPUSortValuePooled[BufferIndex].SafeRelease();
        GPUSortValueBufferRHI[BufferIndex].SafeRelease();
        GPUSortValueSRV[BufferIndex].SafeRelease();
        GPUSortValueUAV[BufferIndex].SafeRelease();
    }

    GPUSortKeyCapacity = 0;
    GPUSortValueCapacity = 0;
}

bool FGaussianSplatSorter::AreSortScratchBuffersValid(int32 SplatCount)
{
    bool IsValid = GPUSortKeyCapacity >= SplatCount && GPUSortValueCapacity >= SplatCount;
    for (int32 BufferIndex = 0; BufferIndex < 2; ++BufferIndex)
    {
        IsValid &= GPUSortKeyPooled[BufferIndex].IsValid();
        IsValid &= GPUSortKeyBufferRHI[BufferIndex].IsValid();
        IsValid &= GPUSortKeySRV[BufferIndex].IsValid();
        IsValid &= GPUSortKeyUAV[BufferIndex].IsValid();

        IsValid &= GPUSortValuePooled[BufferIndex].IsValid();
        IsValid &= GPUSortValueBufferRHI[BufferIndex].IsValid();
        IsValid &= GPUSortValueSRV[BufferIndex].IsValid();
        IsValid &= GPUSortValueUAV[BufferIndex].IsValid();
    }
    return IsValid;
}
