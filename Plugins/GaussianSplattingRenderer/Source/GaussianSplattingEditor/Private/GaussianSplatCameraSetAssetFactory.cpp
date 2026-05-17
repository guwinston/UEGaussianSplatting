#include "GaussianSplatCameraSetAssetFactory.h"

#include "GaussianSplatCameraSetAsset.h"
#include "Editor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Subsystems/ImportSubsystem.h"

UGaussianSplatCameraSetAssetFactory::UGaussianSplatCameraSetAssetFactory()
{
    SupportedClass = UGaussianSplatCameraSetAsset::StaticClass();

    Formats.Add(TEXT("json;3DGS Cameras JSON"));

    bCreateNew = false;
    bEditAfterNew = false;
    bEditorImport = true;
    bText = true;
}

bool UGaussianSplatCameraSetAssetFactory::DoesSupportClass(UClass* Class)
{
    return Class == UGaussianSplatCameraSetAsset::StaticClass();
}

bool UGaussianSplatCameraSetAssetFactory::FactoryCanImport(const FString& Filename)
{
    if (FPaths::GetExtension(Filename).ToLower() != TEXT("json"))
    {
        return false;
    }

    FString FileContents;
    if (!FFileHelper::LoadFileToString(FileContents, *Filename))
    {
        return false;
    }

    return FileContents.Contains(TEXT("\"position\""))
        && FileContents.Contains(TEXT("\"rotation\""))
        && FileContents.Contains(TEXT("\"fx\""))
        && FileContents.Contains(TEXT("\"fy\""));
}

UObject* UGaussianSplatCameraSetAssetFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPreImport(
        this, InClass, InParent, InName, TEXT("json"));

    UGaussianSplatCameraSetAsset* Asset =
        NewObject<UGaussianSplatCameraSetAsset>(InParent, InClass, InName, Flags);
    if (!Asset)
    {
        if (Warn)
        {
            Warn->Logf(ELogVerbosity::Error,
                TEXT("GaussianSplatCameraSetAssetFactory: Failed to create UGaussianSplatCameraSetAsset."));
        }
        return nullptr;
    }

    Asset->SourceJsonPath = Filename;
    const bool bLoaded = Asset->LoadFromCamerasJson(Filename);
    if (!bLoaded && Warn)
    {
        Warn->Logf(ELogVerbosity::Error,
            TEXT("GaussianSplatCameraSetAssetFactory: Failed to parse cameras.json file: %s"), *Filename);
    }

    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPostImport(this, Asset);
    return Asset;
}

bool UGaussianSplatCameraSetAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
    UGaussianSplatCameraSetAsset* Asset = Cast<UGaussianSplatCameraSetAsset>(Obj);
    if (Asset && !Asset->SourceJsonPath.IsEmpty())
    {
        OutFilenames.Add(Asset->SourceJsonPath);
        return true;
    }

    return false;
}

void UGaussianSplatCameraSetAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
    UGaussianSplatCameraSetAsset* Asset = Cast<UGaussianSplatCameraSetAsset>(Obj);
    if (Asset && NewReimportPaths.Num() > 0)
    {
        Asset->SourceJsonPath = NewReimportPaths[0];
    }
}

EReimportResult::Type UGaussianSplatCameraSetAssetFactory::Reimport(UObject* Obj)
{
    UGaussianSplatCameraSetAsset* Asset = Cast<UGaussianSplatCameraSetAsset>(Obj);
    if (!Asset || Asset->SourceJsonPath.IsEmpty())
    {
        return EReimportResult::Failed;
    }

    if (!Asset->LoadFromCamerasJson(Asset->SourceJsonPath))
    {
        UE_LOG(LogTemp, Error,
            TEXT("GaussianSplatCameraSetAssetFactory: Reimport failed for %s"),
            *Asset->SourceJsonPath);
        return EReimportResult::Failed;
    }

    Asset->MarkPackageDirty();
    return EReimportResult::Succeeded;
}

int32 UGaussianSplatCameraSetAssetFactory::GetPriority() const
{
    return ImportPriority;
}
