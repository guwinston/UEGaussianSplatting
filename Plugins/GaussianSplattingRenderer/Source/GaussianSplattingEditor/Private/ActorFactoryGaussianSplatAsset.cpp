#include "ActorFactoryGaussianSplatAsset.h"

#include "GaussianSplatActor.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"

#define LOCTEXT_NAMESPACE "GaussianSplattingActorFactory"

UActorFactoryGaussianSplatAsset::UActorFactoryGaussianSplatAsset(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    DisplayName = LOCTEXT("GaussianSplatAssetDisplayName", "Gaussian Splat Asset");
    NewActorClass = AGaussianSplatActor::StaticClass();
    bShowInEditorQuickMenu = true;
    bUseSurfaceOrientation = true;
    // We register this factory manually via the editor module; prevent duplicate auto-registration.
    bShouldAutoRegister = false;
}

bool UActorFactoryGaussianSplatAsset::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
    if (!AssetData.IsValid() || !AssetData.IsInstanceOf(UGaussianSplatAsset::StaticClass()))
    {
        OutErrorMsg = LOCTEXT("InvalidGaussianSplatAsset", "A valid Gaussian Splat Asset must be specified.");
        return false;
    }

    return true;
}

void UActorFactoryGaussianSplatAsset::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
    Super::PostSpawnActor(Asset, NewActor);

    UGaussianSplatAsset* GaussianSplatAsset = Cast<UGaussianSplatAsset>(Asset);
    AGaussianSplatActor* GaussianActor = Cast<AGaussianSplatActor>(NewActor);
    if (!GaussianSplatAsset || !GaussianActor)
    {
        return;
    }

    GaussianActor->SetGaussianSplatAsset(GaussianSplatAsset);
}

UObject* UActorFactoryGaussianSplatAsset::GetAssetFromActorInstance(AActor* ActorInstance)
{
    AGaussianSplatActor* GaussianActor = Cast<AGaussianSplatActor>(ActorInstance);
    if (!GaussianActor || !GaussianActor->GaussianSplatComponent)
    {
        return nullptr;
    }

    return GaussianActor->GaussianSplatComponent->GaussianSplatAsset;
}

#undef LOCTEXT_NAMESPACE
