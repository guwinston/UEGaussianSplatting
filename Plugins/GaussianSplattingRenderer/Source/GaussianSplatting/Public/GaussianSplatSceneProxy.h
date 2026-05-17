#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "RHI.h"
#include "RHIResources.h"
#include "DynamicMeshBuilder.h"
#include "StaticMeshResources.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"

#if WITH_EDITOR
#include "HitProxies.h"
#endif

class UGaussianSplatComponent;
class FGaussianSplatViewExtension;
class UBodySetup;

/**
 * Render-thread scene proxy for 3D Gaussian Splatting.
 *
 * Lifetime: created on the game thread (via UGaussianSplatComponent::CreateSceneProxy),
 * destroyed on the render thread.
 *
 * Rendering strategy:
 *   GetDynamicMeshElements registers this proxy with FGaussianSplatViewExtension.
 *   The view extension merges ALL active proxies into a SINGLE draw call using a
 *   globally sorted index buffer, ensuring correct alpha compositing even when
 *   multiple Gaussian scenes overlap spatially.
 *
 * CPU data lifetime:
 *   CompressedCPUData is kept alive by shared ownership so the global sorter and
 *   the ViewExtension can read the packed payload each frame without re-expanding
 *   the asset back into float arrays.
 */
class GAUSSIANSPLATTING_API FGaussianSplatSceneProxy : public FPrimitiveSceneProxy
{
public:
    explicit FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent);
    virtual ~FGaussianSplatSceneProxy();

    //~ Begin FPrimitiveSceneProxy Interface
    virtual void GetDynamicMeshElements(
        const TArray<const FSceneView*>& Views,
        const FSceneViewFamily& ViewFamily,
        uint32 VisibilityMap,
        FMeshElementCollector& Collector) const override;

    virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

    virtual SIZE_T GetTypeHash() const override
    {
        static size_t UniquePointer;
        return reinterpret_cast<size_t>(&UniquePointer);
    }

    virtual uint32 GetMemoryFootprint() const override
    {
        return sizeof(*this) + GetAllocatedSize();
    }
    SIZE_T GetAllocatedSize() const { return FPrimitiveSceneProxy::GetAllocatedSize(); }

    virtual void DestroyRenderThreadResources() override;
    //~ End FPrimitiveSceneProxy Interface

    /** Used by UGaussianSplatComponent::CalcBounds */
    FBoxSphereBounds CalcLocalBounds() const;

    /** Returns the render scene this proxy belongs to, used to keep PIE/editor worlds from being merged. */
    const FSceneInterface* GetOwnerSceneInterface() const
    {
        return &GetScene();
    }

    // ---- CPU compressed payload (kept alive for merged packed-buffer building) ----
    TSharedPtr<FGaussianSplatCompressedData> CompressedCPUData;

    // ---- Per-proxy rendering parameters (copied from Component on construction) ----
    float SplatScale         = 1.0f;
    int32 MaxSHDegree        = 3;
    float AlphaCullThreshold = 0.004f;

private:
    FBoxSphereBounds LocalBounds;

    /** Shadow-caster mesh built from either the optional proxy mesh or the generated splat contour. */
    uint32                          ShadowCasterNumTris = 0;
    FStaticMeshVertexBuffers        ShadowCasterVBs;
    FDynamicMeshIndexBuffer32       ShadowCasterIB;
    TUniquePtr<FLocalVertexFactory> ShadowCasterVF;

#if WITH_EDITOR
    /** When true, the editor selection/hit-proxy path reuses the shadow proxy mesh resources. */
    bool bUseShadowCasterForEditorSelection = false;

    /**
     * BodySetup for collision / wireframe overlay in the editor.
     * Cached from the component on construction; only read (no writes) on the render thread.
     */
    UBodySetup* BodySetup = nullptr;

    /**
     * Convex hull mesh used as an invisible hit-proxy volume in the editor.
     */
    uint32                              ConvexHullNumTris = 0;
    FStaticMeshVertexBuffers            ConvexHullVBs;
    FDynamicMeshIndexBuffer32           ConvexHullIB;
    TUniquePtr<FLocalVertexFactory>     ConvexHullVF;
#endif
};
