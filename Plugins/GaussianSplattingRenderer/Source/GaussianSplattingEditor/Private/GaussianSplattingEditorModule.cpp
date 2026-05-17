#include "GaussianSplattingEditorModule.h"
#include "GaussianSplatActorDetails.h"
#include "GaussianSplatAssetTypeActions.h"
#include "GaussianSplatCameraSetAssetFactory.h"
#include "GaussianSplatCameraSetAssetTypeActions.h"
#include "ActorFactoryGaussianSplatAsset.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "FGaussianSplattingEditorModule"

void FGaussianSplattingEditorModule::StartupModule()
{
    // Register a custom asset category "Gaussian Splatting" in the Content Browser
    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    const EAssetTypeCategories::Type GaussianSplattingCategory =
        AssetTools.RegisterAdvancedAssetCategory(
            FName(TEXT("GaussianSplatting")),
            LOCTEXT("GaussianSplattingCategory", "Gaussian Splatting"));

    // Register asset type actions so Content Browser knows about UGaussianSplatAsset
    GaussianSplatAssetTypeActions = MakeShared<FGaussianSplatAssetTypeActions>(GaussianSplattingCategory);
    AssetTools.RegisterAssetTypeActions(GaussianSplatAssetTypeActions.ToSharedRef());

    GaussianSplatCameraSetAssetTypeActions =
        MakeShared<FGaussianSplatCameraSetAssetTypeActions>(GaussianSplattingCategory);
    AssetTools.RegisterAssetTypeActions(GaussianSplatCameraSetAssetTypeActions.ToSharedRef());

    FPropertyEditorModule& PropertyEditorModule =
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
    PropertyEditorModule.RegisterCustomClassLayout(
        TEXT("GaussianSplatActor"),
        FOnGetDetailCustomizationInstance::CreateStatic(&FGaussianSplatActorDetails::MakeInstance));
    PropertyEditorModule.NotifyCustomizationModuleChanged();

    // Register actor factory so dragging GaussianSplatAsset into a level spawns AGaussianSplatActor.
    // We defer to OnPostEngineInit to guarantee GEditor is fully initialized.
    OnPostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
    {
        if (GEditor)
        {
            GaussianSplatActorFactory = NewObject<UActorFactoryGaussianSplatAsset>(GetTransientPackage());
            if (GaussianSplatActorFactory)
            {
                GEditor->ActorFactories.Add(GaussianSplatActorFactory);
                UE_LOG(LogTemp, Log, TEXT("GaussianSplattingEditor: Registered GaussianSplat actor factory."));
            }
        }
    });

    UE_LOG(LogTemp, Log, TEXT("GaussianSplattingEditor: Editor module started."));
}

void FGaussianSplattingEditorModule::ShutdownModule()
{
    // Unregister asset type actions
    if (FModuleManager::Get().IsModuleLoaded("AssetTools") && GaussianSplatAssetTypeActions.IsValid())
    {
        IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
        AssetTools.UnregisterAssetTypeActions(GaussianSplatAssetTypeActions.ToSharedRef());
        if (GaussianSplatCameraSetAssetTypeActions.IsValid())
        {
            AssetTools.UnregisterAssetTypeActions(GaussianSplatCameraSetAssetTypeActions.ToSharedRef());
        }
    } 

    if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
    {
        FPropertyEditorModule& PropertyEditorModule =
            FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
        PropertyEditorModule.UnregisterCustomClassLayout(TEXT("GaussianSplatActor"));
        PropertyEditorModule.NotifyCustomizationModuleChanged();
    }

    FCoreDelegates::OnPostEngineInit.Remove(OnPostEngineInitHandle);

    if (GEditor && GaussianSplatActorFactory)
    {
        GEditor->ActorFactories.Remove(GaussianSplatActorFactory);
        GaussianSplatActorFactory = nullptr;
    }

    UE_LOG(LogTemp, Log, TEXT("GaussianSplattingEditor: Editor module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSplattingEditorModule, GaussianSplattingEditor)
