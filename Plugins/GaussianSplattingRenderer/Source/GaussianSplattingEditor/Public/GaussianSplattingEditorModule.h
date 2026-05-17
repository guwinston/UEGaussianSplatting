#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "AssetTypeCategories.h"
#include "Delegates/IDelegateInstance.h"

class IAssetTypeActions;
class UActorFactory;

class FGaussianSplattingEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedPtr<IAssetTypeActions> GaussianSplatAssetTypeActions;
    TSharedPtr<IAssetTypeActions> GaussianSplatCameraSetAssetTypeActions;
    UActorFactory* GaussianSplatActorFactory = nullptr;
    FDelegateHandle OnPostEngineInitHandle;
};
