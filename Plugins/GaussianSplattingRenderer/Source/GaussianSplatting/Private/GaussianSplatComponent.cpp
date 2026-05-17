#include "GaussianSplatComponent.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatAsset.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"

#if WITH_EDITOR
#include "Engine/Engine.h"
#endif

UGaussianSplatComponent::UGaussianSplatComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bUseAsOccluder = false;
    bSelectable = true;
    bUseEditorCompositing = true;
    CastShadow = true;
    SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetCollisionResponseToAllChannels(ECR_Ignore);
    bCanEverAffectNavigation = false;
#if WITH_EDITOR
    bAlwaysAllowTranslucentSelect = true;
#endif
}

void UGaussianSplatComponent::OnRegister()
{
    Super::OnRegister();
}

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
    if (!GaussianSplatAsset || !GaussianSplatAsset->IsLoaded())
        return nullptr;

    // Ensure BodySetup is created before the proxy reads it.
    // If a shadow proxy mesh is provided we use its body setup for editor
    // selection/collision visualization; otherwise we fall back to the
    // generated selection hull data stored on the Gaussian asset.
    GetBodySetup();

    return new FGaussianSplatSceneProxy(this);
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    FBox LocalBox(ForceInit);
    bool bHasBounds = false;

    if (GaussianSplatAsset && GaussianSplatAsset->IsLoaded())
    {
        LocalBox = GaussianSplatAsset->GetBounds();
        // Grow by 10% to account for splat extents
        LocalBox = LocalBox.ExpandBy(LocalBox.GetExtent() * 0.1f);
        bHasBounds = true;
    }

    if (GaussianSplatAsset && GaussianSplatAsset->ShadowProxyMesh)
    {
        const FBox ProxyBounds = GaussianSplatAsset->ShadowProxyMesh->GetBoundingBox();
        if (ProxyBounds.IsValid)
        {
            LocalBox = bHasBounds ? (LocalBox + ProxyBounds) : ProxyBounds;
            bHasBounds = true;
        }
    }

    if (bHasBounds)
    {
        return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
    }

    return FBoxSphereBounds(FVector::ZeroVector, FVector(100.0f), 100.0f).TransformBy(LocalToWorld);
}

void UGaussianSplatComponent::LoadFromFile(const FString& FilePath)
{
    if (!GaussianSplatAsset)
    {
        GaussianSplatAsset = NewObject<UGaussianSplatAsset>(this, NAME_None, RF_Transient);
    }

    if (GaussianSplatAsset->LoadFromPly(FilePath))
    {
        OnSplatAssetChanged();
    }
}

void UGaussianSplatComponent::OnSplatAssetChanged()
{
    RebuildBodySetup();
    MarkRenderStateDirty();
    UpdateBounds();
}

#if WITH_EDITOR
void UGaussianSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName PropertyName = PropertyChangedEvent.GetPropertyName();
    if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSplatAsset))
    {
        OnSplatAssetChanged();
        return;
    }

    if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxSHDegree)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, AlphaCullThreshold)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UPrimitiveComponent, CastShadow))
    {
        MarkRenderStateDirty();
        UpdateBounds();
    }
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// BodySetup / collision / editor selection
// ─────────────────────────────────────────────────────────────────────────────

void UGaussianSplatComponent::RebuildBodySetup()
{
    BodySetup = GaussianSplatAsset ? GaussianSplatAsset->GetSelectionBodySetup() : nullptr;
}

UBodySetup* UGaussianSplatComponent::GetBodySetup()
{
    if (!GaussianSplatAsset)
    {
        return nullptr;
    }
    else if (!BodySetup)
    {
        RebuildBodySetup();
    }
    return BodySetup;
}

#if WITH_EDITOR
void UGaussianSplatComponent::GetUsedMaterials(
    TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
    if (bGetDebugMaterials && GEngine)
    {
        // These materials are used by the scene proxy for collision/debug views.
        OutMaterials.Add(GEngine->GeomMaterial);
        OutMaterials.Add(GEngine->ShadedLevelColorationUnlitMaterial);
        OutMaterials.Add(GEngine->WireframeMaterial);
    }
}
#endif
