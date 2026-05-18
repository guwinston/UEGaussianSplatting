#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatShaders.h"
#include "SceneViewExtension.h"
#include "GaussianSplatSceneProxy.h"
#include "RHIResources.h"
#include "RenderGraphResources.h"
#include "Misc/EngineVersionComparison.h"

// Forward declarations: avoid including private/renderer headers in this public header.
struct FPostProcessingInputs;
struct FPostProcessMaterialInputs;
struct FScreenPassTexture;
class  FGaussianSplatSorter;

/**
 * Scene View Extension for 3D Gaussian Splatting.
 *
 * Architecture:
 *   - Game thread: UGaussianSplatComponent creates FGaussianSplatSceneProxy.
 *   - GetDynamicMeshElements (render thread): proxy calls RegisterProxy() to
 *     add itself to this extension's per-frame list.
 *   - PrePostProcessPass_RenderThread: ALL registered proxies are merged into a
 *     SINGLE draw call with a globally sorted index buffer.
 *
 * Why single merged draw call:
 *   Per-object sorting only ensures correct ordering within each object.
 *   When two Gaussian scenes spatially overlap, splats from different objects
 *   must be interleaved in global depth order for correct alpha compositing.
 *   A single globally sorted draw call achieves this correctly.
 *
 * Why PrePostProcessPass and not PostRenderBasePassDeferred:
 *   PostRenderBasePassDeferred fires while GBuffer render targets are bound.
 *   The GBuffer SceneColor target stores Emissive + GBuffer data, NOT the
 *   final lit color.  Rendering translucent splats there produces no visible
 *   result in the final image.  PrePostProcessPass fires after lighting is
 *   resolved into SceneColor, so our splats composite correctly.
 *
 * Performance architecture:
 *   Static splat data (packed attributes, per-chunk decode metadata, object-index mapping)
 *   never changes after asset load, so we cache PERSISTENT pooled GPU buffers for each. These
 *   are only rebuilt when the set of active proxies changes.
 *   Every frame only uploads:
 *     - SortedVisibleIndexBuffer + indirect draw args (the GPU sort result)
 *     - PerObjectBuffer    (~K * 192 bytes, transforms + per-object params)
 *   This eliminates the dominant per-frame CPU->GPU bandwidth cost.
 *
 * A single global instance (singleton) is created at module startup.
 */
class GAUSSIANSPLATTING_API FGaussianSplatViewExtension : public FSceneViewExtensionBase
{
public:
    FGaussianSplatViewExtension(const FAutoRegister& AutoRegister);
    virtual ~FGaussianSplatViewExtension();

    // ---- FSceneViewExtensionBase interface ----
    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

    /**
     * Called right before Post Processing rendering begins.
     * At this point SceneColor holds the lit scene (opaque + sky).
     * We render ALL Gaussian splats in a single merged draw call, using a
     * globally sorted index buffer for correct inter-object transparency.
     */
    virtual void PrePostProcessPass_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& InView,
        const FPostProcessingInputs& Inputs) override;

    virtual void SubscribeToPostProcessingPass(
        EPostProcessingPass Pass,
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
        const FSceneView& InView,
#endif
        FAfterPassCallbackDelegateArray& InOutPassCallbacks,
        bool bIsPassEnabled) override;

    /** Register a proxy to be rendered this frame (called from render thread). */
    void RegisterProxy(const FGaussianSplatSceneProxy* Proxy);

    /**
     * Unregister a proxy that is being destroyed.
     * MUST be called on the render thread (from DestroyRenderThreadResources)
     * to prevent dangling pointer access in PrePostProcessPass_RenderThread.
     */
    void UnregisterProxy(const FGaussianSplatSceneProxy* Proxy);

    // ---- Singleton management ----
    static TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> Get();
    static void Create();
    static void Destroy();

private:
    /** Proxies registered for this frame; cleared after rendering. */
    TArray<const FGaussianSplatSceneProxy*> RegisteredProxies;
    FCriticalSection ProxyLock;

    /**
     * Global GPU sorter - culls and sorts ALL splats from ALL active proxies together.
     * Produces a single back-to-front sorted global-splat index array plus indirect draw args.
     */
    TSharedPtr<FGaussianSplatSorter> GlobalSorter;

    // =========================================================
    //  Persistent static merged GPU buffers
    //
    //  Splat geometry (positions, covariances, colors, SH) is
    //  immutable after asset load.  We cache persistent pooled
    //  buffers and only rebuild when the proxy set changes.
    //  Each frame only uploads the per-object descriptor buffer and rebuilds the GPU sort outputs.
    // =========================================================

    /** Proxy set fingerprint for the current cached static buffers. */
    TArray<const FGaussianSplatSceneProxy*> CachedProxySet;

    /** Total splat/chunk/direct-SH-word counts for the cached buffers. */
    int32 CachedTotalSplats   = 0;
    int32 CachedTotalChunks   = 0;
    int32 CachedTotalSHCodebookEntries = 0;
    int32 CachedTotalSHWords  = 0;
    bool bCachedEnableHigherOrderSH = true;

    /** Persistent pooled GPU buffers (backed by TRefCountPtr<FRDGPooledBuffer>). */
    TRefCountPtr<FRDGPooledBuffer> StaticPackedPositionPooled;
    TRefCountPtr<FRDGPooledBuffer> StaticPackedColorPooled;
    TRefCountPtr<FRDGPooledBuffer> StaticPackedRotationPooled;
    TRefCountPtr<FRDGPooledBuffer> StaticPackedScalePooled;
    TRefCountPtr<FRDGPooledBuffer> StaticPackedSHDataPooled;    // may be null if no higher-order SH data
    TRefCountPtr<FRDGPooledBuffer> StaticSHCodebookPooled;       // may be null if no higher-order SH data
    TRefCountPtr<FRDGPooledBuffer> StaticChunkPositionMinPooled;
    TRefCountPtr<FRDGPooledBuffer> StaticChunkPositionMaxPooled;
    TRefCountPtr<FRDGPooledBuffer> StaticObjectIndexPooled;

    /** Per-object splat/chunk/direct-SH offsets into the merged static buffers. */
    TArray<uint32> CachedSplatOffsets;
    TArray<uint32> CachedChunkOffsets;
    TArray<uint32> CachedSHCodebookOffsets;
    TArray<uint32> CachedSHWordOffsets;

    // =========================================================
    //  Persistent dynamic (upload-path) GPU buffers for per-frame data.
    //
    //  The per-object descriptor buffer changes every frame but we
    //  want to avoid RDG re-allocating them from scratch each time.
    //  We keep persistent pooled buffers sized to the maximum needed
    //  and just write into them via LockBuffer each frame.
    // =========================================================

    /** Persistent per-object descriptor buffer. */
    TRefCountPtr<FRDGPooledBuffer> DynPerObjPooled;
    int32 DynPerObjCapacity = 0;  //< Number of uint4 slots allocated

    /** Returns true if ValidProxies matches the current CachedProxySet and buffers are live. */
    bool IsStaticCacheValid(const TArray<const FGaussianSplatSceneProxy*>& ValidProxies) const;

    /** Rebuild all static pooled GPU buffers from ValidProxies (render thread). */
    void RebuildStaticBuffers(
        FRHICommandListImmediate& RHICmdList,
        const TArray<const FGaussianSplatSceneProxy*>& ValidProxies);

    /** Ensure persistent dynamic buffer has at least MinElements capacity (recreate if needed). */
    static void EnsureDynBuffer(
        FRHICommandListImmediate& RHICmdList,
        TRefCountPtr<FRDGPooledBuffer>& InOutPooled,
        int32& InOutCapacity,
        uint32 Stride, int32 MinElements,
        const TCHAR* Name);

    void RenderGaussianSplats_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& InView,
        const TArray<const FGaussianSplatSceneProxy*>& ProxiesToRender,
        FRDGTextureRef SceneColorTexture,
        FRDGTextureRef SceneDepthTexture,
        const FIntRect& ViewRect,
        float CompositePreExposure,
        bool bCompositeToUELinear);

    FScreenPassTexture PostProcessAfterTonemap_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& InView,
        const FPostProcessMaterialInputs& Inputs);

    FScreenPassTexture PostProcessAfterMotionBlur_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& InView,
        const FPostProcessMaterialInputs& Inputs);

    static TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> Instance;
};

