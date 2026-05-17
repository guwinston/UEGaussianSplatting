#include "GaussianSplatAssetFactory.h"
#include "GaussianSplatAsset.h"
#include "Misc/Paths.h"
#include "Misc/FeedbackContext.h"
#include "Editor.h"
#include "Subsystems/ImportSubsystem.h"

UGaussianSplatAssetFactory::UGaussianSplatAssetFactory()
{
    // What type this factory creates
    SupportedClass = UGaussianSplatAsset::StaticClass();

    // File formats handled: "ply;3D Gaussian Splatting Point Cloud"
    Formats.Add(TEXT("ply;3D Gaussian Splatting Point Cloud"));

    // Allow drag-and-drop + Content Browser import
    bCreateNew = false;
    bEditAfterNew = false;
    bEditorImport = true;
    bText = false;
}

bool UGaussianSplatAssetFactory::DoesSupportClass(UClass* Class)
{
    return Class == UGaussianSplatAsset::StaticClass();
}

bool UGaussianSplatAssetFactory::FactoryCanImport(const FString& Filename)
{
    return FPaths::GetExtension(Filename).ToLower() == TEXT("ply");
}

UObject* UGaussianSplatAssetFactory::FactoryCreateFile(
    UClass*           InClass,
    UObject*          InParent,
    FName             InName,
    EObjectFlags      Flags,
    const FString&    Filename,
    const TCHAR*      Parms,
    FFeedbackContext* Warn,
    bool&             bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPreImport(
        this, InClass, InParent, InName, TEXT("ply"));

    UGaussianSplatAsset* Asset = NewObject<UGaussianSplatAsset>(InParent, InClass, InName, Flags);
    if (!Asset)
    {
        if (Warn)
        {
            Warn->Logf(ELogVerbosity::Error,
                TEXT("GaussianSplatAssetFactory: Failed to create UGaussianSplatAsset object."));
        }
        return nullptr;
    }

    // Store the source path for future reimport
    Asset->SourcePlyPath = Filename;

    // Actually load the PLY data into the asset
    const bool bLoaded = Asset->LoadFromPly(Filename);
    if (!bLoaded)
    {
        if (Warn)
        {
            Warn->Logf(ELogVerbosity::Error,
                TEXT("GaussianSplatAssetFactory: Failed to parse PLY file: %s"), *Filename);
        }
        // Return the (empty) asset rather than nullptr so UE doesn't crash;
        // user will see a warning in the output log.
    }
    else
    {
        UE_LOG(LogTemp, Log,
            TEXT("GaussianSplatAssetFactory: Imported %d splats from %s"),
            Asset->SplatCount, *Filename);
    }

    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPostImport(this, Asset);

    return Asset;
}

// ─── FReimportHandler ────────────────────────────────────────────────────────

bool UGaussianSplatAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
    UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
    if (Asset && !Asset->SourcePlyPath.IsEmpty())
    {
        OutFilenames.Add(Asset->SourcePlyPath);
        return true;
    }
    return false;
}

void UGaussianSplatAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
    UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
    if (Asset && NewReimportPaths.Num() > 0)
    {
        Asset->SourcePlyPath = NewReimportPaths[0];
    }
}

EReimportResult::Type UGaussianSplatAssetFactory::Reimport(UObject* Obj)
{
    UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
    if (!Asset || Asset->SourcePlyPath.IsEmpty())
    {
        return EReimportResult::Failed;
    }

    const bool bLoaded = Asset->LoadFromPly(Asset->SourcePlyPath);
    if (!bLoaded)
    {
        UE_LOG(LogTemp, Error,
            TEXT("GaussianSplatAssetFactory: Reimport failed for %s"), *Asset->SourcePlyPath);
        return EReimportResult::Failed;
    }

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatAssetFactory: Reimported %d splats from %s"),
        Asset->SplatCount, *Asset->SourcePlyPath);

    // Mark the package dirty so it gets saved
    Asset->MarkPackageDirty();

    return EReimportResult::Succeeded;
}

int32 UGaussianSplatAssetFactory::GetPriority() const
{
    return ImportPriority;
}
