#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatViewExtension.h"

#include "RHICommandList.h"
#include "RHIResources.h"
#include "RHIDefinitions.h"
#include "DynamicRHI.h"
#include "RenderGraphBuilder.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "DynamicMeshBuilder.h"
#include "PrimitiveViewRelevance.h"
#include "SceneManagement.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"

#if WITH_EDITOR
#include "PhysicsEngine/BodySetup.h"
#include "HitProxies.h"
#endif

namespace
{
// ============================================================
// ExtractStaticMeshLOD0Geometry
// Reads LOD0 geometry from an optional UStaticMesh shadow proxy and
// flattens it into CPU-side vertex/index arrays that can be consumed
// by the scene proxy's custom shadow-caster mesh path.
// Returns false when the mesh or its render data is unavailable.
// ============================================================
static bool ExtractStaticMeshLOD0Geometry(
    const UStaticMesh* StaticMesh,
    TArray<FVector3f>& OutVertices,
    TArray<uint32>& OutIndices)
{
    OutVertices.Reset();
    OutIndices.Reset();

    if (!StaticMesh)
    {
        return false;
    }

    const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
    if (!RenderData || RenderData->LODResources.IsEmpty())
    {
        return false;
    }

    const FStaticMeshLODResources& LODResources = RenderData->LODResources[0];
    const int32 VertexCount = LODResources.VertexBuffers.PositionVertexBuffer.GetNumVertices();
    const FIndexArrayView IndexView = LODResources.IndexBuffer.GetArrayView();
    if (VertexCount <= 0 || IndexView.Num() <= 0)
    {
        return false;
    }

    OutVertices.SetNumUninitialized(VertexCount);
    for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
    {
        OutVertices[VertexIndex] = LODResources.VertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex);
    }

    OutIndices.SetNumUninitialized(IndexView.Num());
    for (int32 Index = 0; Index < IndexView.Num(); ++Index)
    {
        OutIndices[Index] = IndexView[Index];
    }

    return true;
}

// ============================================================
// InitializeDynamicMeshResources
// Converts flat CPU-side vertex/index arrays into render resources
// owned by the scene proxy: vertex buffers, index buffer, local
// vertex factory, and cached triangle count for mesh submission.
// Returns false when the input geometry is empty.
// ============================================================
static bool InitializeDynamicMeshResources(
    ERHIFeatureLevel::Type FeatureLevel,
    const char* DebugName,
    const TArray<FVector3f>& Vertices,
    const TArray<uint32>& Indices,
    FStaticMeshVertexBuffers& OutVertexBuffers,
    FDynamicMeshIndexBuffer32& OutIndexBuffer,
    TUniquePtr<FLocalVertexFactory>& OutVertexFactory,
    uint32& OutNumTriangles)
{
    OutNumTriangles = 0;

    if (Vertices.IsEmpty() || Indices.IsEmpty())
    {
        return false;
    }

    TArray<FDynamicMeshVertex> DynamicVertices;
    DynamicVertices.SetNumUninitialized(Vertices.Num());
    for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
    {
        DynamicVertices[VertexIndex] = FDynamicMeshVertex(Vertices[VertexIndex]);
    }

    OutVertexFactory = MakeUnique<FLocalVertexFactory>(FeatureLevel, DebugName);
    OutVertexBuffers.InitFromDynamicVertex(OutVertexFactory.Get(), DynamicVertices);

    OutIndexBuffer.Indices = Indices;
    BeginInitResource(&OutIndexBuffer);

    OutNumTriangles = Indices.Num() / 3;
    return OutNumTriangles > 0;
}
}

// ============================================================
// FGaussianSplatSceneProxy Constructor
// Caches the packed per-splat payload plus lightweight per-proxy metadata.
// The view extension later builds merged packed GPU buffers from this shared
// compressed asset data, and the shaders decode attributes on demand.
// ============================================================

FGaussianSplatSceneProxy::FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent)
    : FPrimitiveSceneProxy(InComponent, NAME_None)
    , SplatScale(InComponent->SplatScale)
    , MaxSHDegree(InComponent->MaxSHDegree)
    , AlphaCullThreshold(InComponent->AlphaCullThreshold)
{
    bVerifyUsedMaterials = false;

    if (!InComponent->GaussianSplatAsset || !InComponent->GaussianSplatAsset->IsLoaded())
        return;

    // Cache local bounds
    LocalBounds = InComponent->CalcBounds(FTransform::Identity);

    // Take a shared reference to the compressed payload so it stays alive across the render-thread boundary.
    TSharedPtr<FGaussianSplatCompressedData> DataCopy = InComponent->GaussianSplatAsset->CompressedSplatData;
    if (!DataCopy.IsValid() || !DataCopy->IsValid())
        return;

    CompressedCPUData = DataCopy;

    {
        TArray<FVector3f> ShadowCasterVertices;
        TArray<uint32> ShadowCasterIndices;

        bool bHasShadowCasterGeometry = ExtractStaticMeshLOD0Geometry(
            InComponent->GaussianSplatAsset->ShadowProxyMesh,
            ShadowCasterVertices,
            ShadowCasterIndices);

        if (!bHasShadowCasterGeometry)
        {
            const TArray<FVector3f>& FallbackVertices = InComponent->GaussianSplatAsset->SelectionMeshVertices;
            const TArray<uint32>& FallbackIndices = InComponent->GaussianSplatAsset->SelectionMeshIndices;
            if (!FallbackVertices.IsEmpty() && !FallbackIndices.IsEmpty())
            {
                ShadowCasterVertices = FallbackVertices;
                ShadowCasterIndices = FallbackIndices;
                bHasShadowCasterGeometry = true;
            }
        }

        if (bHasShadowCasterGeometry)
        {
            InitializeDynamicMeshResources(
                GetScene().GetFeatureLevel(),
                "FGaussianSplatSceneProxy_ShadowCaster",
                ShadowCasterVertices,
                ShadowCasterIndices,
                ShadowCasterVBs,
                ShadowCasterIB,
                ShadowCasterVF,
                ShadowCasterNumTris);
        }
    }

#if WITH_EDITOR
    // Cache BodySetup pointer from Component
    BodySetup = InComponent->BodySetup;
    bUseShadowCasterForEditorSelection = InComponent->GaussianSplatAsset->ShadowProxyMesh && ShadowCasterVF.IsValid() && ShadowCasterNumTris > 0;

    // Build the generated selection mesh only when we are not explicitly
    // reusing the shadow proxy mesh for editor selection/hit testing.
    if (!bUseShadowCasterForEditorSelection)
    {
        const TArray<FVector3f>& HullVerts = InComponent->GaussianSplatAsset->SelectionMeshVertices;
        const TArray<uint32>& HullIndices = InComponent->GaussianSplatAsset->SelectionMeshIndices;

        if (HullVerts.Num() > 0 && HullIndices.Num() > 0)
        {
            ConvexHullNumTris = HullIndices.Num() / 3;

            TArray<FDynamicMeshVertex> DynVerts;
            DynVerts.SetNumUninitialized(HullVerts.Num());
            for (int32 i = 0; i < HullVerts.Num(); ++i)
            {
                DynVerts[i] = FDynamicMeshVertex(HullVerts[i]);
            }

            ConvexHullVF = MakeUnique<FLocalVertexFactory>(
                GetScene().GetFeatureLevel(), "FGaussianSplatSceneProxy_ConvexHull");
            ConvexHullVBs.InitFromDynamicVertex(ConvexHullVF.Get(), DynVerts);

            ConvexHullIB.Indices.SetNumUninitialized(HullIndices.Num());
            for (int32 i = 0; i < HullIndices.Num(); ++i)
            {
                ConvexHullIB.Indices[i] = HullIndices[i];
            }
            BeginInitResource(&ConvexHullIB);
        }
    }
#endif

}

// ============================================================
// Destructor — release GPU resources on render thread
// ============================================================
FGaussianSplatSceneProxy::~FGaussianSplatSceneProxy()
{
}

// ============================================================
// DestroyRenderThreadResources
// ============================================================
void FGaussianSplatSceneProxy::DestroyRenderThreadResources()
{
    // CRITICAL: remove this proxy from the ViewExtension's render list BEFORE
    // any GPU resources are released.
    TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> Extension = FGaussianSplatViewExtension::Get();
    if (Extension.IsValid())
    {
        Extension->UnregisterProxy(this);
    }

    if (ShadowCasterVF.IsValid())
    {
        ShadowCasterVF->ReleaseResource();
    }
    ShadowCasterVBs.PositionVertexBuffer.ReleaseResource();
    ShadowCasterVBs.StaticMeshVertexBuffer.ReleaseResource();
    ShadowCasterVBs.ColorVertexBuffer.ReleaseResource();
    ShadowCasterIB.ReleaseResource();

#if WITH_EDITOR
    if (ConvexHullVF.IsValid())
    {
        ConvexHullVF->ReleaseResource();
    }
    ConvexHullVBs.PositionVertexBuffer.ReleaseResource();
    ConvexHullVBs.StaticMeshVertexBuffer.ReleaseResource();
    ConvexHullVBs.ColorVertexBuffer.ReleaseResource();
    ConvexHullIB.ReleaseResource();
#endif

    FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

// ============================================================
// GetDynamicMeshElements
// Registers this proxy with the ViewExtension for the current frame.
// ============================================================
void FGaussianSplatSceneProxy::GetDynamicMeshElements(
    const TArray<const FSceneView*>& Views,
    const FSceneViewFamily& ViewFamily,
    uint32 VisibilityMap,
    FMeshElementCollector& Collector) const
{
    TSharedPtr<FGaussianSplatViewExtension, ESPMode::ThreadSafe> Extension = FGaussianSplatViewExtension::Get();
    if (Extension.IsValid() && CompressedCPUData.IsValid() && CompressedCPUData->IsValid())
    {
        Extension->RegisterProxy(this);
    }

    if (ShadowCasterVF.IsValid() && ShadowCasterNumTris > 0)
    {
        const FMaterialRenderProxy* ShadowMaterialProxy =
            UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();

        for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
        {
            if ((VisibilityMap & (1u << ViewIndex)) == 0 || !IsShadowCast(Views[ViewIndex]))
            {
                continue;
            }

            FMeshBatch& Mesh = Collector.AllocateMesh();
            Mesh.VertexFactory = ShadowCasterVF.Get();
            Mesh.MaterialRenderProxy = ShadowMaterialProxy;
            Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
            Mesh.bDisableBackfaceCulling = CastsShadowAsTwoSided();
            Mesh.CastShadow = true;
            Mesh.bUseForMaterial = false;
            Mesh.bUseForDepthPass = false;
            Mesh.bUseAsOccluder = false;
            Mesh.bSelectable = false;
            Mesh.Type = PT_TriangleList;
            Mesh.DepthPriorityGroup = GetDepthPriorityGroup(Views[ViewIndex]);
            Mesh.LODIndex = 0;
            Mesh.SegmentIndex = 0;
            Mesh.MeshIdInPrimitive = 0;
            Mesh.bCanApplyViewModeOverrides = false;

            FMeshBatchElement& BatchElement = Mesh.Elements[0];
            BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
            BatchElement.IndexBuffer = &ShadowCasterIB;
            BatchElement.FirstIndex = 0;
            BatchElement.NumPrimitives = ShadowCasterNumTris;
            BatchElement.MinVertexIndex = 0;
            BatchElement.MaxVertexIndex = ShadowCasterVBs.PositionVertexBuffer.GetNumVertices() - 1;

            Collector.AddMesh(ViewIndex, Mesh);
        }
    }

#if WITH_EDITOR
    const FLocalVertexFactory* EditorSelectionVF = bUseShadowCasterForEditorSelection ? ShadowCasterVF.Get() : ConvexHullVF.Get();
    const FDynamicMeshIndexBuffer32* EditorSelectionIB = bUseShadowCasterForEditorSelection ? &ShadowCasterIB : &ConvexHullIB;
    const uint32 EditorSelectionNumTris = bUseShadowCasterForEditorSelection ? ShadowCasterNumTris : ConvexHullNumTris;

    if (GIsEditor && EditorSelectionVF && EditorSelectionNumTris > 0)
    {
        check(GEngine);

        for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
        {
            const bool bDrawPawnCollision   = ViewFamily.EngineShowFlags.CollisionPawn;
            const bool bDrawCollisionOverlay = ViewFamily.EngineShowFlags.Collision;
            const bool bIsCollisionView     = AllowDebugViewmodes() && IsCollisionEnabled() && bDrawPawnCollision;
            const bool bIsWireframeView     = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;

            if (bIsCollisionView && BodySetup)
            {
                FLinearColor SelectionColor = GetSelectionColor(GetWireframeColor(), IsSelected(), IsHovered());
                const bool bDrawSolid = !bDrawCollisionOverlay;
                UMaterial* Mat = bDrawSolid ? GEngine->ShadedLevelColorationUnlitMaterial : GEngine->WireframeMaterial;
                FColoredMaterialRenderProxy* CollisionProxy = new FColoredMaterialRenderProxy(Mat->GetRenderProxy(), SelectionColor);
                Collector.RegisterOneFrameMaterialProxy(CollisionProxy);
                BodySetup->AggGeom.GetAggGeom(FTransform(GetLocalToWorld()), SelectionColor.ToFColor(false), CollisionProxy, false, bDrawSolid, AlwaysHasVelocity(), ViewIndex, Collector);
            }
            else if (bIsWireframeView)
            {
                FLinearColor WireColor = ViewFamily.EngineShowFlags.ActorColoration ? GetPrimitiveColor() : GetWireframeColor();
                FColoredMaterialRenderProxy* WireProxy = new FColoredMaterialRenderProxy(
                    GEngine->WireframeMaterial->GetRenderProxy(),
                    GetSelectionColor(WireColor, IsSelected(), IsHovered(), false));
                Collector.RegisterOneFrameMaterialProxy(WireProxy);

                FMeshBatch& Mesh = Collector.AllocateMesh();
                Mesh.bDisableBackfaceCulling = true;
                Mesh.LODIndex = 0;
                Mesh.MaterialRenderProxy = WireProxy;
                Mesh.bUseWireframeSelectionColoring = IsSelected();
                Mesh.VertexFactory = EditorSelectionVF;
                Mesh.bWireframe = true;
                Mesh.bSelectable = true;
                Mesh.BatchHitProxyId = FHitProxyId();

                FMeshBatchElement& BE = Mesh.Elements[0];
                BE.FirstIndex  = 0;
                BE.IndexBuffer = EditorSelectionIB;
                BE.NumPrimitives = EditorSelectionNumTris;
                Collector.AddMesh(ViewIndex, Mesh);
            }
            else
            {
                // Normal editor view: invisible mesh for mouse click selection.
                FColoredMaterialRenderProxy* HullProxy = new FColoredMaterialRenderProxy(
                    GEngine->GeomMaterial->GetRenderProxy(),
                    FLinearColor(0, 0, 0, 0));
                Collector.RegisterOneFrameMaterialProxy(HullProxy);
                FMeshBatch& Mesh = Collector.AllocateMesh();
                Mesh.bDisableBackfaceCulling = true;
                Mesh.LODIndex = 0;
                Mesh.MaterialRenderProxy = HullProxy;
                Mesh.VertexFactory = EditorSelectionVF;
                Mesh.bSelectable = true;
                Mesh.BatchHitProxyId = FHitProxyId();

                FMeshBatchElement& BE = Mesh.Elements[0];
                BE.FirstIndex  = 0;
                BE.IndexBuffer = EditorSelectionIB;
                BE.NumPrimitives = EditorSelectionNumTris;
                Collector.AddMesh(ViewIndex, Mesh);
            }
        }
    }
#endif
}

// ============================================================
// GetViewRelevance
// ============================================================
FPrimitiveViewRelevance FGaussianSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
    FPrimitiveViewRelevance Result;
    Result.bShadowRelevance      = IsShadowCast(View);
    Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
    Result.bRenderCustomDepth    = ShouldRenderCustomDepth();

#if WITH_EDITOR
    if (GIsEditor)
    {
        Result.bDrawRelevance    = IsShown(View);
        Result.bDynamicRelevance = true;
        Result.bEditorStaticSelectionRelevance = (IsSelected() || IsHovered());
        return Result;
    }
#endif

    Result.bDrawRelevance      = IsShown(View);
    Result.bDynamicRelevance   = true;
    Result.bRenderInMainPass   = false; // Rendered via ViewExtension
    Result.bOpaque             = false;
    Result.bNormalTranslucency = true;

    return Result;
}

// ============================================================
// CalcLocalBounds
// ============================================================
FBoxSphereBounds FGaussianSplatSceneProxy::CalcLocalBounds() const
{
    return LocalBounds;
}
