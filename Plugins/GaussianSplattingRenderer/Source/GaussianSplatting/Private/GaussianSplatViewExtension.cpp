#include "GaussianSplatViewExtension.h"
#include "GaussianSplatCVars.h"
#include "GaussianSplatShaders.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatSorter.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "RHICommandList.h"
#include "CommonRenderResources.h"       // GEmptyVertexDeclaration
#include "ShaderParameterUtils.h"
#include "PostProcess/PostProcessInputs.h"   // FPostProcessingInputs
#include "PostProcess/PostProcessMaterialInputs.h"
#include "ScreenPass.h"                      // FScreenPassTexture
#include "SceneRendering.h"                  // FViewInfo (for PreExposure access)
#include "RenderTargetPool.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Misc/EngineVersionComparison.h"

// UE 5.4 doesn't have RDG_EVENT_SCOPE_STAT; emulate it with the plain event scope.
// The companion RDG_GPU_STAT_SCOPE call (already present below each usage) covers the GPU stat.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    #ifndef RDG_EVENT_SCOPE_STAT
        #define RDG_EVENT_SCOPE_STAT(GraphBuilder, StatName, Format, ...) \
            RDG_EVENT_SCOPE(GraphBuilder, Format, ##__VA_ARGS__)
    #endif
#endif

DECLARE_GPU_STAT_NAMED(GaussianSplat, TEXT("Gaussian Splat"));
DECLARE_GPU_STAT_NAMED(GaussianSplatAccumulate, TEXT("Gaussian Splat Accumulate"));
DECLARE_GPU_STAT_NAMED(GaussianSplatComposite, TEXT("Gaussian Splat Composite"));
DECLARE_GPU_STAT_NAMED(GaussianSplatDirect, TEXT("Gaussian Splat Direct"));

// File-local helpers used by this view extension, including small utility
// functions and transient data structures for resolving view/light state.
namespace
{
static FScreenPassTexture CreateWritablePostProcessSceneColor(
    FRDGBuilder& GraphBuilder,
    const FPostProcessMaterialInputs& Inputs,
    const TCHAR* OutputName)
{
    const FScreenPassTextureSlice SceneColorSlice = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
    if (!SceneColorSlice.TextureSRV)
    {
        return FScreenPassTexture();
    }

    FRDGTextureRef InputTexture = SceneColorSlice.TextureSRV->Desc.Texture;
    if (!InputTexture)
    {
        return FScreenPassTexture();
    }

    FRDGTextureRef OutputTexture = Inputs.OverrideOutput.Texture;
    if (!OutputTexture)
    {
        FRDGTextureDesc OutputDesc = InputTexture->Desc;
        OutputDesc.Dimension = ETextureDimension::Texture2D;
        OutputDesc.ArraySize = 1;
        OutputDesc.Flags |= ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource;
        OutputTexture = GraphBuilder.CreateTexture(OutputDesc, OutputName);
    }

    FScreenPassTexture Output(OutputTexture, SceneColorSlice.ViewRect);
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    return FScreenPassTexture::CopyFromSlice(GraphBuilder, SceneColorSlice);
#else
    return FScreenPassTexture::CopyFromSlice(GraphBuilder, SceneColorSlice, Output);
#endif
}

}




// ============================================================
//  Singleton storage
// ============================================================
TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> FGaussianSplatViewExtension::Instance;

// ============================================================
//  Constructor / Destructor
// ============================================================
FGaussianSplatViewExtension::FGaussianSplatViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
    GlobalSorter = MakeShared<FGaussianSplatSorter>();
    if (!GlobalSorter->Initialize())
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatting: Failed to initialize the global GPU sorter."));
        GlobalSorter.Reset();
    }
}

FGaussianSplatViewExtension::~FGaussianSplatViewExtension()
{
    if (GlobalSorter.IsValid())
    {
        GlobalSorter->Shutdown();
        GlobalSorter.Reset();
    }
    StochasticTemporalHistories.Empty();
}

// ============================================================
//  Proxy registration
// ============================================================
void FGaussianSplatViewExtension::RegisterProxy(const FGaussianSplatSceneProxy* Proxy)
{
    FScopeLock Lock(&ProxyLock);
    RegisteredProxies.AddUnique(Proxy);
}

void FGaussianSplatViewExtension::UnregisterProxy(const FGaussianSplatSceneProxy* Proxy)
{
    FScopeLock Lock(&ProxyLock);
    RegisteredProxies.Remove(Proxy);

    // Invalidate static cache so it rebuilds next frame without this proxy.
    CachedProxySet.Empty();
    StochasticTemporalHistories.Empty();
}

// ============================================================
//  Singleton management
// ============================================================
TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> FGaussianSplatViewExtension::Get()
{
    return Instance;
}

void FGaussianSplatViewExtension::Create()
{
    if (!Instance.IsValid())
        Instance = FSceneViewExtensions::NewExtension<FGaussianSplatViewExtension>();
}

void FGaussianSplatViewExtension::Destroy()
{
    Instance.Reset();
}

// ============================================================
//  IsStaticCacheValid
// ============================================================
bool FGaussianSplatViewExtension::IsStaticCacheValid(
    const TArray<const FGaussianSplatSceneProxy*>& ValidProxies) const
{
    const bool bEnableHigherOrderSH = GaussianSplatCVars::GetEnableHigherOrderSHOnRenderThread() != 0;
    if (bCachedEnableHigherOrderSH != bEnableHigherOrderSH)
        return false;
    if (ValidProxies.Num() != CachedProxySet.Num())
        return false;
    for (int32 i = 0; i < ValidProxies.Num(); ++i)
        if (ValidProxies[i] != CachedProxySet[i])
            return false;
    return StaticPackedPositionPooled.IsValid()
        && StaticPackedColorPooled.IsValid()
        && StaticPackedRotationPooled.IsValid()
        && StaticPackedScalePooled.IsValid()
        && StaticChunkPositionMinPooled.IsValid()
        && StaticChunkPositionMaxPooled.IsValid()
        && StaticObjectIndexPooled.IsValid()
        && (!bEnableHigherOrderSH
            || CachedTotalSHWords == 0
            || (StaticPackedSHDataPooled.IsValid() && StaticSHCodebookPooled.IsValid()));
}

// ============================================================
//  RebuildStaticBuffers
//
//  Creates (or recreates) persistent pooled GPU buffers holding the merged
//  packed splat attributes, per-chunk decode metadata and object-index data
//  for all active proxies. Called only when the proxy set changes.
// ============================================================
void FGaussianSplatViewExtension::RebuildStaticBuffers(
    FRHICommandListImmediate& RHICmdList,
    const TArray<const FGaussianSplatSceneProxy*>& ValidProxies)
{
    const bool bEnableHigherOrderSH = GaussianSplatCVars::GetEnableHigherOrderSHOnRenderThread() != 0;
    int32 TotalSplats = 0;
    int32 TotalChunks = 0;
    int32 TotalSHCodebookEntries = 0;
    int32 TotalSHWords = 0;

    CachedSplatOffsets.SetNumUninitialized(ValidProxies.Num());
    CachedChunkOffsets.SetNumUninitialized(ValidProxies.Num());
    CachedSHCodebookOffsets.SetNumUninitialized(ValidProxies.Num());
    CachedSHWordOffsets.SetNumUninitialized(ValidProxies.Num());

    for (int32 i = 0; i < ValidProxies.Num(); ++i)
    {
        const FGaussianSplatSceneProxy* P = ValidProxies[i];
        const FGaussianSplatCompressedData& Data = *P->CompressedCPUData;

        CachedSplatOffsets[i] = (uint32)TotalSplats;
        CachedChunkOffsets[i] = (uint32)TotalChunks;
        CachedSHCodebookOffsets[i] = (uint32)TotalSHCodebookEntries;
        CachedSHWordOffsets[i] = (uint32)TotalSHWords;

        TotalSplats += Data.SplatCount;
        TotalChunks += Data.GetChunkCount();
        if (bEnableHigherOrderSH)
        {
            TotalSHCodebookEntries += Data.SHCodebook.Num();
            TotalSHWords += Data.PackedSHData.Num();
        }
    }

    CachedTotalSplats = TotalSplats;
    CachedTotalChunks = TotalChunks;
    CachedTotalSHCodebookEntries = TotalSHCodebookEntries;
    CachedTotalSHWords = TotalSHWords;

    TArray<uint16> MergedPackedPositions;
    TArray<uint32> MergedPackedColors;
    TArray<uint32> MergedPackedRotations;
    TArray<uint32> MergedPackedScales;
    TArray<uint32> MergedPackedSHData;
    TArray<float> MergedSHCodebook;
    TArray<FVector4f> MergedChunkPositionMins;
    TArray<FVector4f> MergedChunkPositionMaxs;
    TArray<uint32> MergedObjectIndices;

    MergedPackedPositions.SetNumUninitialized(TotalSplats * 3);
    MergedPackedColors.SetNumUninitialized(TotalSplats);
    MergedPackedRotations.SetNumUninitialized(TotalSplats);
    MergedPackedScales.SetNumUninitialized(TotalSplats);
    MergedObjectIndices.SetNumUninitialized(TotalSplats);
    MergedChunkPositionMins.SetNumUninitialized(TotalChunks);
    MergedChunkPositionMaxs.SetNumUninitialized(TotalChunks);
    if (bEnableHigherOrderSH && TotalSHWords > 0)
    {
        MergedPackedSHData.SetNumUninitialized(TotalSHWords);
        MergedSHCodebook.SetNumUninitialized(TotalSHCodebookEntries);
    }

    for (int32 i = 0; i < ValidProxies.Num(); ++i)
    {
        const FGaussianSplatSceneProxy* P = ValidProxies[i];
        const FGaussianSplatCompressedData& Data = *P->CompressedCPUData;
        const int32 N = Data.SplatCount;
        const uint32 SplatOffset = CachedSplatOffsets[i];
        const uint32 ChunkOffset = CachedChunkOffsets[i];
        const uint32 SHCodebookOffset = CachedSHCodebookOffsets[i];
        const uint32 SHWordOffset = CachedSHWordOffsets[i];

        FMemory::Memcpy(MergedPackedPositions.GetData() + SplatOffset * 3, Data.PackedPositions.GetData(), N * 3 * sizeof(uint16));
        FMemory::Memcpy(MergedPackedColors.GetData() + SplatOffset, Data.PackedColors.GetData(), N * sizeof(uint32));
        FMemory::Memcpy(MergedPackedRotations.GetData() + SplatOffset, Data.PackedRotations.GetData(), N * sizeof(uint32));
        FMemory::Memcpy(MergedPackedScales.GetData() + SplatOffset, Data.PackedScales.GetData(), N * sizeof(uint32));
        const int32 ChunkCount = Data.GetChunkCount(); 
        FMemory::Memcpy(MergedChunkPositionMins.GetData() + ChunkOffset, Data.ChunkPositionMins.GetData(), ChunkCount * sizeof(FVector4f));
        FMemory::Memcpy(MergedChunkPositionMaxs.GetData() + ChunkOffset, Data.ChunkPositionMaxs.GetData(), ChunkCount * sizeof(FVector4f));

        if (bEnableHigherOrderSH && !Data.PackedSHData.IsEmpty())
        {
            FMemory::Memcpy(MergedPackedSHData.GetData() + SHWordOffset, Data.PackedSHData.GetData(), Data.PackedSHData.Num() * sizeof(uint32));
            FMemory::Memcpy(MergedSHCodebook.GetData() + SHCodebookOffset, Data.SHCodebook.GetData(), Data.SHCodebook.Num() * sizeof(float));
        }

        for (int32 LocalSplatIndex = 0; LocalSplatIndex < N; ++LocalSplatIndex)
        {
            MergedObjectIndices[SplatOffset + LocalSplatIndex] = (uint32)i;
        }
    }

    // ---- Helper lambda: create/replace a persistent static GPU buffer and wrap it in FRDGPooledBuffer ----
    // We keep both structured buffers (float data streams) and plain typed buffers (uint indices).
    // The RHI creation path and RDG descriptor must match the shader-side buffer type.
    auto UploadStaticBufferToPooled = [&](
        TRefCountPtr<FRDGPooledBuffer>& InOutPooled,
        uint32 Stride, uint32 NumElements,
        const void* SrcData, uint32 SrcBytes,
        const TCHAR* Name,
        bool bStructured)
    {
        const uint32 BufSize = Stride * NumElements;
        FRHIResourceCreateInfo CI(Name);
        const EBufferUsageFlags RHIFlags = bStructured
            ? (EBufferUsageFlags::Static
                | EBufferUsageFlags::UnorderedAccess
                | EBufferUsageFlags::ShaderResource
                | EBufferUsageFlags::StructuredBuffer)
            : (EBufferUsageFlags::Static
                | EBufferUsageFlags::ShaderResource);

        FBufferRHIRef RHIBuf = bStructured
            ? RHICmdList.CreateStructuredBuffer(Stride, BufSize, RHIFlags, ERHIAccess::SRVMask, CI)
            : RHICmdList.CreateVertexBuffer(BufSize, RHIFlags, CI);

        void* Dst = RHICmdList.LockBuffer(RHIBuf, 0, BufSize, RLM_WriteOnly);
        FMemory::Memcpy(Dst, SrcData, SrcBytes);
        RHICmdList.UnlockBuffer(RHIBuf);

        FRDGBufferDesc Desc = bStructured
            ? FRDGBufferDesc::CreateStructuredDesc(Stride, NumElements)
            : FRDGBufferDesc::CreateBufferDesc(Stride, NumElements);
        Desc.Usage = RHIFlags;
        InOutPooled = new FRDGPooledBuffer(TRefCountPtr<FRHIBuffer>(RHIBuf), Desc, NumElements, Name);
    };

    UploadStaticBufferToPooled(StaticPackedPositionPooled,
        sizeof(uint16), TotalSplats * 3,
        MergedPackedPositions.GetData(), TotalSplats * 3 * sizeof(uint16),
        TEXT("GS_StaticPackedPositions"),
        false);
    UploadStaticBufferToPooled(StaticPackedColorPooled,
        sizeof(uint32), TotalSplats,
        MergedPackedColors.GetData(), TotalSplats * sizeof(uint32),
        TEXT("GS_StaticPackedColors"),
        true);
    UploadStaticBufferToPooled(StaticPackedRotationPooled,
        sizeof(uint32), TotalSplats,
        MergedPackedRotations.GetData(), TotalSplats * sizeof(uint32),
        TEXT("GS_StaticPackedRotations"),
        true);
    UploadStaticBufferToPooled(StaticPackedScalePooled,
        sizeof(uint32), TotalSplats,
        MergedPackedScales.GetData(), TotalSplats * sizeof(uint32),
        TEXT("GS_StaticPackedScales"),
        true);
    UploadStaticBufferToPooled(StaticChunkPositionMinPooled,
        sizeof(FVector4f), TotalChunks,
        MergedChunkPositionMins.GetData(), TotalChunks * sizeof(FVector4f),
        TEXT("GS_StaticChunkPositionMins"),
        true);
    UploadStaticBufferToPooled(StaticChunkPositionMaxPooled,
        sizeof(FVector4f), TotalChunks,
        MergedChunkPositionMaxs.GetData(), TotalChunks * sizeof(FVector4f),
        TEXT("GS_StaticChunkPositionMaxs"),
        true);
    UploadStaticBufferToPooled(StaticObjectIndexPooled,
        sizeof(uint32), TotalSplats,
        MergedObjectIndices.GetData(), TotalSplats * sizeof(uint32),
        TEXT("GS_StaticObjectIndices"),
        false);

    if (bEnableHigherOrderSH && TotalSHWords > 0)
    {
        UploadStaticBufferToPooled(StaticPackedSHDataPooled,
            sizeof(uint32), TotalSHWords,
            MergedPackedSHData.GetData(), TotalSHWords * sizeof(uint32),
            TEXT("GS_StaticPackedSHData"),
            true);
        UploadStaticBufferToPooled(StaticSHCodebookPooled,
            sizeof(float), TotalSHCodebookEntries,
            MergedSHCodebook.GetData(), TotalSHCodebookEntries * sizeof(float),
            TEXT("GS_StaticSHCodebook"),
            true);
    }
    else
    {
        StaticPackedSHDataPooled.SafeRelease();
        StaticSHCodebookPooled.SafeRelease();
    }

    CachedProxySet = ValidProxies;
    bCachedEnableHigherOrderSH = bEnableHigherOrderSH;

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatting: Rebuilt static merged buffers. %d proxies, %d total splats."),
        ValidProxies.Num(), TotalSplats);
}

// ============================================================
//  EnsureDynBuffer
//
//  Ensures the persistent dynamic buffer has at least MinElements capacity.
//  Uses Dynamic usage flags so that LockBuffer (CPU write) is efficient.
//  If current buffer is large enough, no-op (no reallocation).
// ============================================================
void FGaussianSplatViewExtension::EnsureDynBuffer(
    FRHICommandListImmediate& RHICmdList,
    TRefCountPtr<FRDGPooledBuffer>& InOutPooled,
    int32& InOutCapacity,
    uint32 Stride,
    int32 MinElements,
    const TCHAR* Name)
{
    if (InOutCapacity >= MinElements && InOutPooled.IsValid())
        return;  // Already large enough — reuse existing buffer

    // Grow with 25% headroom to reduce re-allocations on incremental growth.
    const int32 NewCapacity = MinElements + MinElements / 4 + 16;
    const uint32 BufSize = Stride * (uint32)NewCapacity;

    FRHIResourceCreateInfo CI(Name);

    // Dynamic | ShaderResource | StructuredBuffer: optimal for CPU-write / GPU-read.
    const EBufferUsageFlags RHIFlags =
        EBufferUsageFlags::Dynamic
        | EBufferUsageFlags::ShaderResource
        | EBufferUsageFlags::StructuredBuffer;

    FBufferRHIRef RHIBuf = RHICmdList.CreateStructuredBuffer(
        Stride, BufSize,
        RHIFlags,
        ERHIAccess::SRVMask,
        CI);

    FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(Stride, NewCapacity);
    // Override usage to match: Dynamic does not include UAV/UnorderedAccess.
    Desc.Usage = EBufferUsageFlags::Dynamic
               | EBufferUsageFlags::ShaderResource
               | EBufferUsageFlags::StructuredBuffer;

    InOutPooled   = new FRDGPooledBuffer(TRefCountPtr<FRHIBuffer>(RHIBuf), Desc, NewCapacity, Name);
    InOutCapacity = NewCapacity;
}

// ============================================================
//  PrePostProcessPass_RenderThread
// ============================================================
void FGaussianSplatViewExtension::RenderGaussianSplats_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& InView,
    const TArray<const FGaussianSplatSceneProxy*>& ProxiesToRender,
    FRDGTextureRef SceneColorTexture,
    FRDGTextureRef SceneDepthTexture,
    const FIntRect& ViewRect,
    float CompositePreExposure,
    bool bCompositeToUELinear)
{
    RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplat, "GaussianSplat");
    RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplat);
    // Compute UAVs, GPU radix sorting and indirect drawing require SM5. Guard
    // before shader lookup so an ES3.1 fallback device safely keeps SceneColor.
    if (InView.GetFeatureLevel() < ERHIFeatureLevel::SM5)
    {
        static bool bLoggedUnsupportedFeatureLevel = false;
        if (!bLoggedUnsupportedFeatureLevel)
        {
            bLoggedUnsupportedFeatureLevel = true;
            UE_LOG(LogTemp, Warning,
                TEXT("GaussianSplatting: the current raster backend requires SM5; the Mobile Vulkan instance-stream backend is not active yet."));
        }
        return;
    }

    // 0. Gather proxies — snapshot for this frame; do NOT clear RegisteredProxies.
    // RegisteredProxies is a persistent set managed by RegisterProxy/UnregisterProxy.
    // Clearing it here would cause the static cache to be invalidated every frame
    // and proxies to disappear from the next frame's render.
    if (ProxiesToRender.IsEmpty())
        return;

    // 1. CVars + shaders
    const int32 RasterMode = GaussianSplatCVars::GetRasterModeOnRenderThread();
    const int32 SortMethod = GaussianSplatCVars::GetSortMethodOnRenderThread();
    const bool bUseStochasticSplat = SortMethod == 2;
    if (!bUseStochasticSplat && !StochasticTemporalHistories.IsEmpty())
    {
        StochasticTemporalHistories.Empty();
    }
    const uint32 bEnableAntialiasing = GaussianSplatCVars::GetEnableAntialiasingOnRenderThread() != 0 ? 1u : 0u;
    const uint32 bEnableOpacityAwareBounds = GaussianSplatCVars::GetOpacityAwareBoundsOnRenderThread() != 0 ? 1u : 0u;
    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
    if (!ShaderMap) return;


    FGaussianSplatVS::FPermutationDomain VsPerm;
    FGaussianSplatMS::FPermutationDomain MsPerm;
    FGaussianSplatPS::FPermutationDomain PsPerm;
    TShaderMapRef<FGaussianSplatVS> VertexShader(ShaderMap, FGaussianSplatVS::FPermutationDomain{});
    TShaderRef<FGaussianSplatMS> MeshShader;
    TShaderMapRef<FGaussianSplatPS> PixelShader(ShaderMap, FGaussianSplatPS::FPermutationDomain{});
    TShaderMapRef<FGaussianSplatCompositeVS> CompositeVertexShader(ShaderMap);
    TShaderMapRef<FGaussianSplatCompositePS> CompositePixelShader(ShaderMap);
    TShaderMapRef<FGaussianSplatTemporalPS> TemporalPixelShader(ShaderMap);
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_SetupShaders_CPU);
        VsPerm.Set<FGaussianSplatRasterModeDim>(RasterMode);
        MsPerm.Set<FGaussianSplatRasterModeDim>(RasterMode);
        PsPerm.Set<FGaussianSplatRasterModeDim>(RasterMode);
        VsPerm.Set<FGaussianSplatStochasticDim>(bUseStochasticSplat);
        MsPerm.Set<FGaussianSplatStochasticDim>(bUseStochasticSplat);
        PsPerm.Set<FGaussianSplatStochasticDim>(bUseStochasticSplat);

        VertexShader = TShaderMapRef<FGaussianSplatVS>(ShaderMap, VsPerm);
        if (GRHISupportsMeshShadersTier0)
        {
            MeshShader = TShaderMapRef<FGaussianSplatMS>(ShaderMap, MsPerm);
        }
        PixelShader = TShaderMapRef<FGaussianSplatPS>(ShaderMap, PsPerm);
        CompositeVertexShader = TShaderMapRef<FGaussianSplatCompositeVS>(ShaderMap);
        CompositePixelShader = TShaderMapRef<FGaussianSplatCompositePS>(ShaderMap);
    }
    if (!VertexShader.IsValid() || !PixelShader.IsValid()
        || !CompositeVertexShader.IsValid() || !CompositePixelShader.IsValid()
        || (bUseStochasticSplat && !TemporalPixelShader.IsValid()))
    {
        return;
    }
    const int32 GeometryMode = GaussianSplatCVars::GetGeometryModeOnRenderThread();
    const bool bUseMeshShader = GeometryMode == 1
        && GRHISupportsMeshShadersTier0
        && MeshShader.IsValid();

    // 2. Camera / viewport
    const FMatrix44d& ViewMatrix = InView.ViewMatrices.GetViewMatrix();
    const FMatrix44d& InvViewMatrix = InView.ViewMatrices.GetInvViewMatrix();
    const bool bUseNoAAProjection = GaussianSplatCVars::GetUseNoAAProjectionOnRenderThread() != 0;
    const FMatrix44d ProjMatrix = bUseNoAAProjection
        ? InView.ViewMatrices.ComputeProjectionNoAAMatrix()
        : InView.ViewMatrices.GetProjectionMatrix();
    const FVector3f   CameraPos  = (FVector3f)InView.ViewMatrices.GetViewOrigin();
    const float VMinX = (float)ViewRect.Min.X;
    const float VMinY = (float)ViewRect.Min.Y;
    const float VMaxX = (float)ViewRect.Max.X;
    const float VMaxY = (float)ViewRect.Max.Y;
    const float VW    = (float)ViewRect.Width();
    const float VH    = (float)ViewRect.Height();
    const float FX    = (float)(ProjMatrix.M[0][0] * 0.5 * VW);
    const float FY    = (float)(ProjMatrix.M[1][1] * 0.5 * VH);

    const FMatrix44f WorldToView = FMatrix44f(ViewMatrix);
    const FMatrix44f ViewToClip  = FMatrix44f(ProjMatrix);
    const FMatrix44f CurrentWorldToClip = FMatrix44f(ViewMatrix * ProjMatrix);
    const FMatrix44f CurrentUnjitteredWorldToClip = FMatrix44f(ViewMatrix * InView.ViewMatrices.ComputeProjectionNoAAMatrix());

    TArray<const FGaussianSplatSceneProxy*> ValidProxies;
    ValidProxies.Reserve(ProxiesToRender.Num());
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_FilterValidProxies_CPU);
        const FSceneInterface* ViewScene = InView.Family ? InView.Family->Scene : nullptr;
        for (const FGaussianSplatSceneProxy* Proxy : ProxiesToRender)
        {
            if (!Proxy || !Proxy->CompressedCPUData.IsValid() || !Proxy->CompressedCPUData->IsValid()) continue;
            if (Proxy->CompressedCPUData->SplatCount <= 0) continue;
            if (ViewScene && Proxy->GetOwnerSceneInterface() != ViewScene) continue;
            ValidProxies.Add(Proxy);
        }
    }

    if (ValidProxies.IsEmpty()) return;

    // Keep proxy order stable across frames so the static object-index buffer and the
    // per-frame object descriptor buffer agree on objectIndex.
    ValidProxies.Sort([](const FGaussianSplatSceneProxy& A, const FGaussianSplatSceneProxy& B)
    {
        return &A < &B;
    });

    // 3. Rebuild static buffers if proxy set changed (heavy: only on scene change)
    bool bRebuiltStaticBuffers = false;
    if (!IsStaticCacheValid(ValidProxies))
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_RebuildStaticBuffers_CPU);
        RebuildStaticBuffers(GraphBuilder.RHICmdList, ValidProxies);
        bRebuiltStaticBuffers = true;
    }

    const int32 TotalSplats = CachedTotalSplats;
    if (TotalSplats <= 0) return;

    const int32 NumObjects = ValidProxies.Num();
    // 4. Build PerObjectBuffer (per-frame: transforms may change)
    static_assert(sizeof(FGaussianSplatObjectGPUDesc) % 16 == 0,
        "FGaussianSplatObjectGPUDesc must be 16-byte aligned");

    TArray<FGaussianSplatObjectGPUDesc> ObjectDescs;
    ObjectDescs.SetNumZeroed(NumObjects);
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_BuildPerObjectDescs_CPU);
        const bool bEnableHigherOrderSH = GaussianSplatCVars::GetEnableHigherOrderSHOnRenderThread() != 0;
        for (int32 i = 0; i < NumObjects; ++i)
        {
            const FGaussianSplatSceneProxy* P = ValidProxies[i];
            const FBox LocalBoundsBox = P->CalcLocalBounds().GetBox();
            FGaussianSplatObjectGPUDesc& D = ObjectDescs[i];
            D.LocalToWorld         = FMatrix44f(P->GetLocalToWorld());
            D.WorldToLocal         = FMatrix44f(P->GetLocalToWorld().InverseFast());
            D.SplatOffset          = CachedSplatOffsets[i];
            D.SplatCount           = (uint32)P->CompressedCPUData->SplatCount;
            D.ChunkOffset          = CachedChunkOffsets[i];
            D.SHDataOffset         = CachedSHWordOffsets[i];
            D.MaxSHDegree          = bEnableHigherOrderSH
                && !P->CompressedCPUData->PackedSHData.IsEmpty()
                ? (uint32)FMath::Min(P->MaxSHDegree, P->CompressedCPUData->SHDegree)
                : 0u;
            D.SHDegree             = (uint32)P->CompressedCPUData->SHDegree;
            D.SHCoefficientsPerChannel = (uint32)P->CompressedCPUData->SHCoefficientsPerChannel;
            D.SHPackedWordsPerSplat = (uint32)P->CompressedCPUData->SHPackedWordsPerSplat;
            D.SplatScale           = P->SplatScale;
            D.AlphaCullThreshold   = P->AlphaCullThreshold;
            D._Pad0 = 0.0f;
            D.ColorQuantMinX       = P->CompressedCPUData->ColorQuantMin.X;
            D.LocalBoundsMin       = FVector3f(LocalBoundsBox.Min);
            D.ColorQuantMinY       = P->CompressedCPUData->ColorQuantMin.Y;
            D.LocalBoundsMax       = FVector3f(LocalBoundsBox.Max);
            D.ColorQuantMinZ       = P->CompressedCPUData->ColorQuantMin.Z;
            D._Pad1 = FVector4f::Zero();
            D.ColorQuantMax = P->CompressedCPUData->ColorQuantMax;
            D.ScaleCodebookOffset = 0;
            D.SHCodebookOffset = CachedSHCodebookOffsets[i];
        }
    }

    // Prepare per-view stochastic history before sort/draw resources are queued.
    const int32 StochasticTemporalSampleLimit = bUseStochasticSplat
        ? GaussianSplatCVars::GetStochasticTemporalSamplesOnRenderThread() : 0;
    const bool bUseStochasticReprojection = StochasticTemporalSampleLimit > 0
        && GaussianSplatCVars::GetStochasticReprojectionOnRenderThread() != 0;
    const uint32 StochasticMotionSampleLimit = static_cast<uint32>(
        GaussianSplatCVars::GetStochasticMotionSamplesOnRenderThread());
    TSharedPtr<FStochasticTemporalHistory> TemporalHistory;
    FMatrix44f PreviousWorldToClip = CurrentWorldToClip;
    bool bStochasticCameraChanged = false;
    uint32 StochasticSampleIndex = InView.Family
        ? static_cast<uint32>(InView.Family->FrameNumber) : 0u;
    if (StochasticTemporalSampleLimit > 0)
    {
        const void* HistoryKey = InView.State
            ? static_cast<const void*>(InView.State)
            : static_cast<const void*>(InView.Family);
        if (HistoryKey)
        {
            TSharedPtr<FStochasticTemporalHistory>& HistorySlot = StochasticTemporalHistories.FindOrAdd(HistoryKey);
            if (!HistorySlot.IsValid())
            {
                HistorySlot = MakeShared<FStochasticTemporalHistory>();
            }
            TemporalHistory = HistorySlot;

            const uint32 CurrentCameraSignature = FCrc::MemCrc32(
                &CurrentUnjitteredWorldToClip, sizeof(CurrentUnjitteredWorldToClip));
            uint32 HistorySignature = FCrc::MemCrc32(
                ObjectDescs.GetData(), ObjectDescs.Num() * sizeof(FGaussianSplatObjectGPUDesc));
            for (const FGaussianSplatSceneProxy* Proxy : ValidProxies)
            {
                HistorySignature = HashCombine(HistorySignature, GetTypeHash(reinterpret_cast<UPTRINT>(Proxy)));
            }
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(RasterMode));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(GeometryMode));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(bEnableAntialiasing));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(bEnableOpacityAwareBounds));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(StochasticTemporalSampleLimit));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(GaussianSplatCVars::GetCullModeOnAnyThread()));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(GaussianSplatCVars::GetSplatFrustumSlackOnAnyThread()));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(GaussianSplatCVars::GetScreenSizeCullOnAnyThread()));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(GaussianSplatCVars::GetScreenSizeCullMinPixelsOnAnyThread()));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(bCompositeToUELinear));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(bUseStochasticReprojection));
            HistorySignature = HashCombine(HistorySignature, GetTypeHash(StochasticMotionSampleLimit));
            if (!bUseStochasticReprojection)
            {
                // Preserve the original behavior when reprojection is disabled: a camera
                // change becomes a history-signature change and restarts accumulation.
                HistorySignature = HashCombine(HistorySignature, CurrentCameraSignature);
            }

            const bool bResetHistory = bRebuiltStaticBuffers
                || TemporalHistory->Signature != HistorySignature
                || TemporalHistory->Extent != SceneColorTexture->Desc.Extent
                || TemporalHistory->ViewRect != ViewRect
                || InView.bCameraCut;
            if (bResetHistory)
            {
                TemporalHistory->Texture.SafeRelease();
                TemporalHistory->SampleCount = 0;
                TemporalHistory->Signature = HistorySignature;
                TemporalHistory->CameraSignature = CurrentCameraSignature;
                TemporalHistory->Extent = SceneColorTexture->Desc.Extent;
                TemporalHistory->ViewRect = ViewRect;
                TemporalHistory->PreviousWorldToClip = CurrentWorldToClip;
                TemporalHistory->bHasPreviousCamera = true;
            }
            else
            {
                bStochasticCameraChanged = TemporalHistory->CameraSignature != CurrentCameraSignature;
            }
            PreviousWorldToClip = TemporalHistory->bHasPreviousCamera
                ? TemporalHistory->PreviousWorldToClip : CurrentWorldToClip;
            if (!bUseStochasticReprojection)
            {
                StochasticSampleIndex = TemporalHistory->SampleCount;
            }
        }
    }

    // 5. Queue the merged GPU sort request for this frame.
    if (GlobalSorter.IsValid())
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_SortSubmitConsume_CPU);
        // Compute tan(HalfFOV) from the projection matrix:
        //   ProjMatrix.M[0][0] = 1 / tan(HalfFovX)  →  TanHalfFovX = 1 / M[0][0]
        //   ProjMatrix.M[1][1] = 1 / tan(HalfFovY)  →  TanHalfFovY = 1 / M[1][1]
        // Guard against degenerate matrices (e.g. ortho projection where M[0][0] could be tiny).
        const float TanHalfFovX = (FMath::Abs((float)ProjMatrix.M[0][0]) > 1e-6f)
            ? 1.0f / (float)ProjMatrix.M[0][0] : 1.0f;
        const float TanHalfFovY = (FMath::Abs((float)ProjMatrix.M[1][1]) > 1e-6f)
            ? 1.0f / (float)ProjMatrix.M[1][1] : 1.0f;

        const bool bForceSortEveryFrame = GaussianSplatCVars::GetForceSortEveryFrameOnAnyThread() != 0;
        GlobalSorter->RequestSort(
            WorldToView,
            ViewToClip,
            TanHalfFovX,
            TanHalfFovY,
            FMath::Max(FMath::Abs(FX), FMath::Abs(FY)),
            TotalSplats,
            /*bLazy=*/!bRebuiltStaticBuffers && !bForceSortEveryFrame);
    }

    // 6. Prepare per-frame draw resources.
    //    This stage assembles the transient draw inputs for the merged splat pass.
    //    Persistent pooled buffers are registered into RDG here, and transient SRVs are rebuilt per frame.

    FRHICommandListImmediate& RHICmdListNow = GraphBuilder.RHICmdList;
    FRDGBufferSRVRef PerObjSRV;
    FRDGBufferSRVRef SortedIndexSRV;
    FRDGBufferSRVRef VisibleCountSRV;
    FRDGBufferSRVRef PackedPosSRV;
    FRDGBufferSRVRef PackedColorSRV;
    FRDGBufferSRVRef PackedRotationSRV;
    FRDGBufferSRVRef PackedScaleSRV;
    FRDGBufferSRVRef PackedSHDataSRV;
    FRDGBufferSRVRef SHCodebookSRV;
    FRDGBufferSRVRef ChunkPositionMinSRV;
    FRDGBufferSRVRef ChunkPositionMaxSRV;
    FRDGBufferSRVRef ObjectIndexSRV;
    FRDGTextureRef GaussianAccumTexture = nullptr;
    const bool bRenderToAccumTexture = bCompositeToUELinear || bUseStochasticSplat;
    FRDGBufferRef DrawIndirectArgsBuffer = nullptr;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_UploadDynamicBuffers_CPU);
        // 6.1 Upload the per-frame per-object descriptor buffer.
        const int32 PerObjUint4Count  = (int32)(NumObjects * sizeof(FGaussianSplatObjectGPUDesc) / 16);
        const uint32 PerObjByteSize   = (uint32)(NumObjects * sizeof(FGaussianSplatObjectGPUDesc));
        EnsureDynBuffer(RHICmdListNow, DynPerObjPooled, DynPerObjCapacity,
            16, PerObjUint4Count, TEXT("GS_DynPerObject"));

        {
            TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_UploadPerObjectBuffer_RT);
            FRHIBuffer* RawBuf = DynPerObjPooled->GetRHI();
            void* Dst = RHICmdListNow.LockBuffer(RawBuf, 0, PerObjByteSize, RLM_WriteOnly);
            FMemory::Memcpy(Dst, ObjectDescs.GetData(), PerObjByteSize);
            RHICmdListNow.UnlockBuffer(RawBuf);
        }
        FRDGBufferRef  PerObjBuf = GraphBuilder.RegisterExternalBuffer(DynPerObjPooled, TEXT("GS_PerObjectBuffer"));
        PerObjSRV = GraphBuilder.CreateSRV(PerObjBuf);
        FShaderResourceViewRHIRef PerObjRHISRV = RHICmdListNow.CreateShaderResourceView(
            DynPerObjPooled->GetRHI(),
            FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));

        if (!GlobalSorter.IsValid())
        {
            return;
        }

        // 6.2 Consume the latest sorter result and prepare the sorted draw buffers.
        FGaussianSplatSortedDrawBuffers SortBuffers;
        {
            TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_PrepareSortedBuffers_RT);
            FRDGBufferRef ExternalPos = GraphBuilder.RegisterExternalBuffer(StaticPackedPositionPooled, TEXT("GS_PackedPositions"));
            FRDGBufferRef ExternalColor = GraphBuilder.RegisterExternalBuffer(StaticPackedColorPooled, TEXT("GS_PackedColors"));
            FRDGBufferRef ExternalRotation = GraphBuilder.RegisterExternalBuffer(StaticPackedRotationPooled, TEXT("GS_PackedRotations"));
            FRDGBufferRef ExternalScale = GraphBuilder.RegisterExternalBuffer(StaticPackedScalePooled, TEXT("GS_PackedScales"));
            FRDGBufferRef ExternalChunkPosMin = GraphBuilder.RegisterExternalBuffer(StaticChunkPositionMinPooled, TEXT("GS_ChunkPositionMins"));
            FRDGBufferRef ExternalChunkPosMax = GraphBuilder.RegisterExternalBuffer(StaticChunkPositionMaxPooled, TEXT("GS_ChunkPositionMaxs"));
            FRDGBufferRef ExternalObj = GraphBuilder.RegisterExternalBuffer(StaticObjectIndexPooled, TEXT("GS_ObjectIndices"));

            PackedPosSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ExternalPos, PF_R16_UINT));
            PackedColorSRV = GraphBuilder.CreateSRV(ExternalColor);
            PackedRotationSRV = GraphBuilder.CreateSRV(ExternalRotation);
            PackedScaleSRV = GraphBuilder.CreateSRV(ExternalScale);
            ChunkPositionMinSRV = GraphBuilder.CreateSRV(ExternalChunkPosMin);
            ChunkPositionMaxSRV = GraphBuilder.CreateSRV(ExternalChunkPosMax);
            ObjectIndexSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ExternalObj, PF_R32_UINT));

            FShaderResourceViewRHIRef PositionRHISRV = RHICmdListNow.CreateShaderResourceView(
                StaticPackedPositionPooled->GetRHI(),
                sizeof(uint16),
                PF_R16_UINT);
            FShaderResourceViewRHIRef ColorRHISRV = RHICmdListNow.CreateShaderResourceView(
                StaticPackedColorPooled->GetRHI(),
                FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));
            FShaderResourceViewRHIRef ScaleRHISRV = RHICmdListNow.CreateShaderResourceView(
                StaticPackedScalePooled->GetRHI(),
                FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));
            FShaderResourceViewRHIRef ChunkPositionMinRHISRV = RHICmdListNow.CreateShaderResourceView(
                StaticChunkPositionMinPooled->GetRHI(),
                FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));
            FShaderResourceViewRHIRef ChunkPositionMaxRHISRV = RHICmdListNow.CreateShaderResourceView(
                StaticChunkPositionMaxPooled->GetRHI(),
                FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured));
            FShaderResourceViewRHIRef ObjectIndexRHISRV = RHICmdListNow.CreateShaderResourceView(
                StaticObjectIndexPooled->GetRHI(),
                sizeof(uint32),
                PF_R32_UINT);

            if (!GlobalSorter->TryConsumeSorted(
                GraphBuilder,
                RHICmdListNow,
                PositionRHISRV,
                ColorRHISRV,
                ScaleRHISRV,
                ChunkPositionMinRHISRV,
                ChunkPositionMaxRHISRV,
                ObjectIndexRHISRV,
                PerObjRHISRV,
                NumObjects,
                SortBuffers))
            {
                return;
            }

            SortedIndexSRV = SortBuffers.SortedIndexSRV;
            VisibleCountSRV = SortBuffers.VisibleCountSRV;
            DrawIndirectArgsBuffer = SortBuffers.DrawIndirectArgsBuffer;

            if (StaticPackedSHDataPooled.IsValid() && StaticSHCodebookPooled.IsValid())
            {
                FRDGBufferRef ExternalSHData = GraphBuilder.RegisterExternalBuffer(StaticPackedSHDataPooled, TEXT("GS_PackedSHData"));
                FRDGBufferRef ExternalSHCodebook = GraphBuilder.RegisterExternalBuffer(StaticSHCodebookPooled, TEXT("GS_SHCodebook"));
                PackedSHDataSRV = GraphBuilder.CreateSRV(ExternalSHData);
                SHCodebookSRV = GraphBuilder.CreateSRV(ExternalSHCodebook);
            }
            else
            {
                PackedSHDataSRV = PackedColorSRV; // dummy (shader skips SH when MaxSHDegree==0)
                SHCodebookSRV = PackedColorSRV; // dummy (shader skips SH when MaxSHDegree==0)
            }
        }

        // 6.4 Create the accumulation target used by the pre-tonemap composite path.
        if (bRenderToAccumTexture)
        {
            FRDGTextureDesc GaussianAccumDesc = FRDGTextureDesc::Create2D(
                SceneColorTexture->Desc.Extent,
                PF_FloatRGBA,
                FClearValueBinding(FLinearColor::Transparent),
                TexCreate_ShaderResource | TexCreate_RenderTargetable);
            GaussianAccumTexture = GraphBuilder.CreateTexture(GaussianAccumDesc, TEXT("GS_CurrentSampleColor"));
            AddClearRenderTargetPass(GraphBuilder, GaussianAccumTexture, FLinearColor::Transparent, ViewRect);
        }
    }

    FRDGTextureRef GaussianStochasticMotionTexture = nullptr;
    if (bUseStochasticReprojection)
    {
        const FRDGTextureDesc MotionDesc = FRDGTextureDesc::Create2D(
            SceneColorTexture->Desc.Extent,
            PF_FloatRGBA,
            FClearValueBinding(FLinearColor::Transparent),
            TexCreate_ShaderResource | TexCreate_RenderTargetable);
        GaussianStochasticMotionTexture = GraphBuilder.CreateTexture(MotionDesc, TEXT("GS_StochasticMotion"));
    }

    FRDGTextureRef GaussianStochasticDepthTexture = nullptr;
    if (bUseStochasticSplat)
    {
        const FRDGTextureDesc StochasticDepthDesc = FRDGTextureDesc::Create2D(
            SceneColorTexture->Desc.Extent,
            PF_DepthStencil,
            FClearValueBinding::DepthFar,
            TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);
        GaussianStochasticDepthTexture = GraphBuilder.CreateTexture(StochasticDepthDesc, TEXT("GS_StochasticDepth"));
    }
    if (!SortedIndexSRV || !VisibleCountSRV || !DrawIndirectArgsBuffer) return;


    const FViewInfo* ViewInfo = InView.bIsViewInfo ? &static_cast<const FViewInfo&>(InView) : nullptr;
    const FIntRect SceneDepthViewRect = ViewInfo ? ViewInfo->ViewRect : ViewRect;
    const bool bUseManualSceneDepthTest = SceneDepthTexture != nullptr
        && (bUseStochasticSplat || (ViewInfo && SceneDepthViewRect != ViewRect));

    // 7. Build accumulation pass parameters and issue the merged splat draw call.
    FGaussianSplatPassParameters* PassParams = GraphBuilder.AllocParameters<FGaussianSplatPassParameters>();
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_SetupAccumulatePass_CPU);
        PassParams->RenderTargets[0] = FRenderTargetBinding(
            bRenderToAccumTexture ? GaussianAccumTexture : SceneColorTexture,
            bRenderToAccumTexture ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);
        if (GaussianStochasticMotionTexture)
        {
            PassParams->RenderTargets[1] = FRenderTargetBinding(
                GaussianStochasticMotionTexture, ERenderTargetLoadAction::EClear);
        }
        if (bUseStochasticSplat)
        {
            PassParams->RenderTargets.DepthStencil = FDepthStencilBinding(
                GaussianStochasticDepthTexture,
                ERenderTargetLoadAction::EClear,
                ERenderTargetLoadAction::ENoAction,
                FExclusiveDepthStencil::DepthWrite_StencilNop);
        }
        else if (SceneDepthTexture && !bUseManualSceneDepthTest)
        {
            // Read the existing scene depth but do not write 3DGS depth back.
            // This lets 3DGS be occluded by already-rendered UE geometry without
            // corrupting the scene depth buffer that later passes may still rely on.
            PassParams->RenderTargets.DepthStencil = FDepthStencilBinding(
                SceneDepthTexture,
                ERenderTargetLoadAction::ELoad,
                ERenderTargetLoadAction::ENoAction,
                FExclusiveDepthStencil::DepthRead_StencilNop);
        }

        PassParams->VS.GlobalPackedPositionBuffer = PackedPosSRV;
        PassParams->VS.GlobalPackedColorBuffer = PackedColorSRV;
        PassParams->VS.GlobalPackedRotationBuffer = PackedRotationSRV;
        PassParams->VS.GlobalPackedScaleBuffer = PackedScaleSRV;
        PassParams->VS.GlobalPackedSHDataBuffer = PackedSHDataSRV;
        PassParams->VS.GlobalSHCodebookBuffer = SHCodebookSRV;
        PassParams->VS.GlobalChunkPositionMinBuffer = ChunkPositionMinSRV;
        PassParams->VS.GlobalChunkPositionMaxBuffer = ChunkPositionMaxSRV;
        PassParams->VS.GlobalObjectIndexBuffer = ObjectIndexSRV;
        PassParams->VS.PerObjectBuffer        = PerObjSRV;
        PassParams->VS.SortedVisibleIndexBuffer = SortedIndexSRV;
        PassParams->VS.VisibleCountBuffer = VisibleCountSRV;
        PassParams->DrawIndirectArgsBuffer = DrawIndirectArgsBuffer;

        PassParams->VS.WorldToView      = WorldToView;
        PassParams->VS.ViewToClip       = ViewToClip;
        PassParams->VS.PreviousWorldToClip = PreviousWorldToClip;
        PassParams->VS.CameraPosition   = CameraPos;
        PassParams->VS.FocalLength      = FVector2f(FX, FY);
        PassParams->VS.ViewportMin      = FVector2f(VMinX, VMinY);
        PassParams->VS.ViewportSize     = FVector2f(VW, VH);
        PassParams->VS.TotalSplatCount  = TotalSplats;
        PassParams->VS.ObjectCount      = NumObjects;
        PassParams->VS.EnableAntialiasing = bEnableAntialiasing;
        PassParams->VS.EnableOpacityAwareBounds = bEnableOpacityAwareBounds;
        PassParams->VS.PreExposure      = CompositePreExposure;
        PassParams->PS.WorldToView = WorldToView;
        PassParams->PS.FocalLength = FVector2f(FX, FY);
        PassParams->PS.ViewportMin = FVector2f(VMinX, VMinY);
        PassParams->PS.ViewportSize = FVector2f(VW, VH);
        PassParams->PS.SceneDepthTexture = SceneDepthTexture ? SceneDepthTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
        PassParams->PS.SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParams->PS.SceneDepthViewportMin = FVector2f((float)SceneDepthViewRect.Min.X, (float)SceneDepthViewRect.Min.Y);
        PassParams->PS.SceneDepthViewportSize = FVector2f((float)SceneDepthViewRect.Width(), (float)SceneDepthViewRect.Height());
        PassParams->PS.SceneDepthTextureExtentInverse = SceneDepthTexture
            ? FVector2f(1.0f / (float)SceneDepthTexture->Desc.Extent.X, 1.0f / (float)SceneDepthTexture->Desc.Extent.Y)
            : FVector2f::ZeroVector;
        PassParams->PS.UseManualSceneDepthTest = bUseManualSceneDepthTest ? 1u : 0u;
        PassParams->PS.StochasticFrameIndex = StochasticSampleIndex;
        if (ViewInfo)
        {
            PassParams->VS.View = ViewInfo->ViewUniformBuffer;
            PassParams->PS.View = ViewInfo->ViewUniformBuffer;
        }
    }

    const TCHAR* GeometryBackendName = bUseMeshShader ? TEXT("MS") : TEXT("VS");
    auto AddMainSplatPass = [&](const TCHAR* PassName)
    {
        GraphBuilder.AddPass(
            RDG_EVENT_NAME("%s_%s", PassName, GeometryBackendName),
            PassParams,
            ERDGPassFlags::Raster,
            [VertexShader, MeshShader, PixelShader, PassParams, VMinX, VMinY, VMaxX, VMaxY, bCompositeToUELinear, bUseManualSceneDepthTest, bUseMeshShader, bUseStochasticSplat, bHasSceneDepth = (SceneDepthTexture != nullptr)]
            (FRHICommandList& RHICmdList)
            {
                FGraphicsPipelineStateInitializer PSOInit;
                RHICmdList.ApplyCachedRenderTargets(PSOInit);

                // Only depth-test when a matching scene-depth target is bound. In the
                // post-tonemap path with screen percentage < 100%, SceneColor may already
                // be upscaled while scene depth is still primary-resolution, which clips
                // rasterization to the smaller overlap region.
                PSOInit.DepthStencilState = bUseStochasticSplat
                    ? TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI()
                    : (bHasSceneDepth && !bUseManualSceneDepthTest)
                    ? TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()
                    : TStaticDepthStencilState<false, CF_Always>::GetRHI();
                if (bCompositeToUELinear || bUseStochasticSplat)
                {
                    PSOInit.BlendState = TStaticBlendState<
                        CW_RGBA,
                        BO_Add, BF_One, BF_InverseSourceAlpha,
                        BO_Add, BF_One,         BF_InverseSourceAlpha>::GetRHI();
                }
                else
                {
                    // After tonemap we can blend the splat result directly into SceneColor,
                    // so RGB uses premultiplied alpha-over while SceneColor alpha is left untouched.
                    PSOInit.BlendState = TStaticBlendState<
                        CW_RGB,
                        BO_Add, BF_One, BF_InverseSourceAlpha,
                        BO_Add, BF_One,         BF_InverseSourceAlpha>::GetRHI();
                }
                PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

                if (bUseMeshShader)
                {
                    PSOInit.BoundShaderState.SetMeshShader(MeshShader.GetMeshShader());
                }
                else
                {
                    PSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
                    PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
                }
                PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
                PSOInit.PrimitiveType = PT_TriangleList;

                SetGraphicsPipelineState(RHICmdList, PSOInit, 0);

                RHICmdList.SetViewport(VMinX, VMinY, 0.0f, VMaxX, VMaxY, 1.0f);
                if (bUseMeshShader)
                {
                    SetShaderParameters(RHICmdList, MeshShader, MeshShader.GetMeshShader(), PassParams->VS);
                }
                else
                {
                    SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParams->VS);
                }
                SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParams->PS);
                if (bUseMeshShader)
                {
                    RHICmdList.DispatchIndirectMeshShader(
                        PassParams->DrawIndirectArgsBuffer->GetIndirectRHICallBuffer(),
                        sizeof(FRHIDrawIndirectParameters));
                }
                else
                {
                    RHICmdList.DrawPrimitiveIndirect(PassParams->DrawIndirectArgsBuffer->GetIndirectRHICallBuffer(), 0);
                }
            }
        );
    };

    if (bRenderToAccumTexture)
    {
        {
            RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplatAccumulate, "GaussianSplat_Accumulate");
            RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatAccumulate);
            AddMainSplatPass(TEXT("GaussianSplat_Accumulate"));
        }

        // 8. Composite the fully accumulated 3DGS image into UE SceneColor.
        FGaussianSplatCompositePassParameters* CompositeParams = GraphBuilder.AllocParameters<FGaussianSplatCompositePassParameters>();
        {
            TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_SetupCompositePass_CPU);
            if (bUseStochasticSplat && TemporalHistory.IsValid())
            {
                const bool bHistoryValid = TemporalHistory->Texture.IsValid()
                    && TemporalHistory->SampleCount > 0;
                const bool bHistoryConverged = bHistoryValid
                    && !bStochasticCameraChanged
                    && TemporalHistory->SampleCount >= static_cast<uint32>(StochasticTemporalSampleLimit);

                if (bHistoryConverged)
                {
                    GaussianAccumTexture = GraphBuilder.RegisterExternalTexture(
                        TemporalHistory->Texture, TEXT("GS_StochasticHistoryConverged"));
                }
                else
                {
                    FRDGTextureRef HistoryInput = bHistoryValid
                        ? GraphBuilder.RegisterExternalTexture(TemporalHistory->Texture, TEXT("GS_StochasticHistoryInput"))
                        : GSystemTextures.GetBlackDummy(GraphBuilder);
                    FRDGTextureDesc HistoryDesc = FRDGTextureDesc::Create2D(
                        SceneColorTexture->Desc.Extent,
                        PF_FloatRGBA,
                        FClearValueBinding(FLinearColor::Transparent),
                        TexCreate_ShaderResource | TexCreate_RenderTargetable);
                    FRDGTextureRef HistoryOutput = GraphBuilder.CreateTexture(HistoryDesc, TEXT("GS_StochasticHistoryOutput"));

                    FGaussianSplatTemporalPassParameters* TemporalParams =
                        GraphBuilder.AllocParameters<FGaussianSplatTemporalPassParameters>();
                    TemporalParams->RenderTargets[0] = FRenderTargetBinding(
                        HistoryOutput, ERenderTargetLoadAction::EClear);
                    TemporalParams->PS.CurrentSampleTexture = GaussianAccumTexture;
                    TemporalParams->PS.HistoryTexture = HistoryInput;
                    TemporalParams->PS.MotionTexture = GaussianStochasticMotionTexture
                        ? GaussianStochasticMotionTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
                    TemporalParams->PS.HistoryTextureSampler =
                        TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                    const uint32 EffectivePriorSamples = bStochasticCameraChanged && bUseStochasticReprojection
                        ? FMath::Min(TemporalHistory->SampleCount, StochasticMotionSampleLimit - 1u)
                        : TemporalHistory->SampleCount;
                    TemporalParams->PS.CurrentSampleWeight = 1.0f
                        / static_cast<float>(EffectivePriorSamples + 1u);
                    TemporalParams->PS.TextureExtentInverse = FVector2f(
                        1.0f / static_cast<float>(SceneColorTexture->Desc.Extent.X),
                        1.0f / static_cast<float>(SceneColorTexture->Desc.Extent.Y));
                    TemporalParams->PS.ViewportMin = FVector2f(VMinX, VMinY);
                    TemporalParams->PS.ViewportSize = FVector2f(VW, VH);
                    TemporalParams->PS.HistoryValid = bHistoryValid ? 1u : 0u;
                    TemporalParams->PS.UseReprojection = bUseStochasticReprojection ? 1u : 0u;

                    GraphBuilder.AddPass(
                        RDG_EVENT_NAME("GaussianSplat_StochasticTemporal_%u", TemporalHistory->SampleCount + 1u),
                        TemporalParams,
                        ERDGPassFlags::Raster,
                        [CompositeVertexShader, TemporalPixelShader, TemporalParams, VMinX, VMinY, VMaxX, VMaxY]
                        (FRHICommandList& RHICmdList)
                        {
                            FGraphicsPipelineStateInitializer PSOInit;
                            RHICmdList.ApplyCachedRenderTargets(PSOInit);
                            PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
                            PSOInit.BlendState = TStaticBlendState<>::GetRHI();
                            PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
                            PSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
                            PSOInit.BoundShaderState.VertexShaderRHI = CompositeVertexShader.GetVertexShader();
                            PSOInit.BoundShaderState.PixelShaderRHI = TemporalPixelShader.GetPixelShader();
                            PSOInit.PrimitiveType = PT_TriangleList;
                            SetGraphicsPipelineState(RHICmdList, PSOInit, 0);
                            RHICmdList.SetViewport(VMinX, VMinY, 0.0f, VMaxX, VMaxY, 1.0f);
                            SetShaderParameters(
                                RHICmdList, TemporalPixelShader, TemporalPixelShader.GetPixelShader(), TemporalParams->PS);
                            RHICmdList.DrawPrimitive(0, 1, 1);
                        });

                    GaussianAccumTexture = HistoryOutput;
                    TemporalHistory->Texture.SafeRelease();
                    GraphBuilder.QueueTextureExtraction(HistoryOutput, &TemporalHistory->Texture);
                    if (bStochasticCameraChanged && bUseStochasticReprojection)
                    {
                        TemporalHistory->SampleCount = FMath::Min(
                            EffectivePriorSamples + 1u, StochasticMotionSampleLimit);
                    }
                    else
                    {
                        TemporalHistory->SampleCount = FMath::Min<uint32>(
                            TemporalHistory->SampleCount + 1u,
                            static_cast<uint32>(StochasticTemporalSampleLimit));
                    }
                }

                TemporalHistory->PreviousWorldToClip = CurrentWorldToClip;
                TemporalHistory->CameraSignature = FCrc::MemCrc32(
                    &CurrentUnjitteredWorldToClip, sizeof(CurrentUnjitteredWorldToClip));
                TemporalHistory->bHasPreviousCamera = true;
            }

            CompositeParams->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
            CompositeParams->PS.GaussianAccumTexture = GaussianAccumTexture;
            CompositeParams->PS.PreExposure = CompositePreExposure;
            CompositeParams->PS.ConvertOutputToLinear = bCompositeToUELinear ? 1u : 0u;
            if (ViewInfo)
            {
                CompositeParams->PS.View = ViewInfo->ViewUniformBuffer;
            }
        }

        {
            RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplatComposite, "GaussianSplat_Composite");
            RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatComposite);
            GraphBuilder.AddPass(
                RDG_EVENT_NAME("GaussianSplat_Composite"),
                CompositeParams,
                ERDGPassFlags::Raster,
                [CompositeVertexShader, CompositePixelShader, CompositeParams, VMinX, VMinY, VMaxX, VMaxY]
                (FRHICommandList& RHICmdList)
                {
                FGraphicsPipelineStateInitializer PSOInit;
                RHICmdList.ApplyCachedRenderTargets(PSOInit);

                PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
                // Composite premultiplied 3DGS contribution over the existing UE scene color.
                // RGB already includes alpha; A carries the accumulated opacity.
                PSOInit.BlendState = TStaticBlendState<
                    CW_RGBA,
                    BO_Add, BF_One, BF_InverseSourceAlpha,
                    BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
                PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

                PSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
                PSOInit.BoundShaderState.VertexShaderRHI = CompositeVertexShader.GetVertexShader();
                PSOInit.BoundShaderState.PixelShaderRHI = CompositePixelShader.GetPixelShader();
                PSOInit.PrimitiveType = PT_TriangleList;

                SetGraphicsPipelineState(RHICmdList, PSOInit, 0);

                RHICmdList.SetViewport(VMinX, VMinY, 0.0f, VMaxX, VMaxY, 1.0f);
                SetShaderParameters(RHICmdList, CompositePixelShader, CompositePixelShader.GetPixelShader(), CompositeParams->PS);
                // Draw one oversized fullscreen triangle; the VS expands 3 vertices
                // to cover the whole viewport, so a second triangle is unnecessary.
                    RHICmdList.DrawPrimitive(0, 1, 1);
                }
            );
        }
    }
    else
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_SetupDirectPass_CPU);
        {
            RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplatDirect, "GaussianSplat_DirectAfterTonemap");
            RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplatDirect);
            AddMainSplatPass(TEXT("GaussianSplat_DirectAfterTonemap"));
        }
    }
}

void FGaussianSplatViewExtension::PrePostProcessPass_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& InView,
    const FPostProcessingInputs& Inputs)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_PrePostProcessPass_CPU);

    if (GaussianSplatCVars::GetRenderModeOnRenderThread() != 0)
    {
        return;
    }

    // RenderMode 0 is composited from the MotionBlur after-pass instead.
    // That callback sits after TAA/TSR and MotionBlur, but still before Tonemap.
    return;
}

void FGaussianSplatViewExtension::SubscribeToPostProcessingPass(
    EPostProcessingPass Pass,
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
    const FSceneView& InView,
#endif
    FAfterPassCallbackDelegateArray& InOutPassCallbacks,
    bool bIsPassEnabled)
{
    if (Pass == EPostProcessingPass::MotionBlur)
    {
        InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
            this, &FGaussianSplatViewExtension::PostProcessAfterMotionBlur_RenderThread));
    }

    if (Pass == EPostProcessingPass::Tonemap && bIsPassEnabled)
    {
        InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
            this, &FGaussianSplatViewExtension::PostProcessAfterTonemap_RenderThread));
    }
}

FScreenPassTexture FGaussianSplatViewExtension::PostProcessAfterMotionBlur_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& InView,
    const FPostProcessMaterialInputs& Inputs)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_PostMotionBlur_CPU);

    if (GaussianSplatCVars::GetRenderModeOnRenderThread() != 0)
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    TArray<const FGaussianSplatSceneProxy*> ProxiesToRender;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_SnapshotProxies_CPU);
        FScopeLock Lock(&ProxyLock);
        ProxiesToRender = RegisteredProxies;
    }

    if (ProxiesToRender.IsEmpty())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    FScreenPassTexture SceneColor = CreateWritablePostProcessSceneColor(
        GraphBuilder,
        Inputs,
        TEXT("GaussianSplat_PostMotionBlurSceneColor"));
    if (!SceneColor.IsValid())
    {
        return SceneColor;
    }

    const FViewInfo* ViewInfo = InView.bIsViewInfo ? &static_cast<const FViewInfo&>(InView) : nullptr;
    RenderGaussianSplats_RenderThread(
        GraphBuilder,
        InView,
        ProxiesToRender,
        SceneColor.Texture,
        ViewInfo ? ViewInfo->GetSceneTextures().Depth.Resolve : nullptr,
        SceneColor.ViewRect,
        ViewInfo ? ViewInfo->PreExposure : 1.0f,
        true);

    return SceneColor;
}

FScreenPassTexture FGaussianSplatViewExtension::PostProcessAfterTonemap_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& InView,
    const FPostProcessMaterialInputs& Inputs)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_PostTonemap_CPU);

    if (GaussianSplatCVars::GetRenderModeOnRenderThread() != 1)
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    TArray<const FGaussianSplatSceneProxy*> ProxiesToRender;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GaussianSplat_SnapshotProxies_CPU);
        FScopeLock Lock(&ProxyLock);
        ProxiesToRender = RegisteredProxies;
    }

    if (ProxiesToRender.IsEmpty())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    FScreenPassTexture SceneColor = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    if (!SceneColor.IsValid())
    {
        return SceneColor;
    }

    RenderGaussianSplats_RenderThread(
        GraphBuilder,
        InView,
        ProxiesToRender,
        SceneColor.Texture,
        InView.bIsViewInfo ? static_cast<const FViewInfo&>(InView).GetSceneTextures().Depth.Resolve : nullptr,
        SceneColor.ViewRect,
        1.0f,
        false);

    return SceneColor;
}
