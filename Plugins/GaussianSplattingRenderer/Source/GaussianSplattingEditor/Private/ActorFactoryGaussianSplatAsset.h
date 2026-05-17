#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "ActorFactoryGaussianSplatAsset.generated.h"

/**
 * Spawns AGaussianSplatActor when a UGaussianSplatAsset is dragged into a level.
 */
UCLASS()
class GAUSSIANSPLATTINGEDITOR_API UActorFactoryGaussianSplatAsset : public UActorFactory
{
    GENERATED_BODY()

public:
    UActorFactoryGaussianSplatAsset(const FObjectInitializer& ObjectInitializer);

    //~ Begin UActorFactory Interface
    virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
    virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
    virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
    //~ End UActorFactory Interface
};
