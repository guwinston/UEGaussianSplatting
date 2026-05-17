#pragma once

#include "CoreMinimal.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "GaussianSplatCameraSetAssetFactory.generated.h"

/**
 * Factory that imports original 3DGS cameras.json files into UGaussianSplatCameraSetAsset.
 */
UCLASS(hidecategories = Object)
class GAUSSIANSPLATTINGEDITOR_API UGaussianSplatCameraSetAssetFactory
    : public UFactory
    , public FReimportHandler
{
    GENERATED_BODY()

public:
    UGaussianSplatCameraSetAssetFactory();

    //~ Begin UFactory Interface
    virtual UObject* FactoryCreateFile(
        UClass* InClass,
        UObject* InParent,
        FName InName,
        EObjectFlags Flags,
        const FString& Filename,
        const TCHAR* Parms,
        FFeedbackContext* Warn,
        bool& bOutOperationCanceled) override;

    virtual bool DoesSupportClass(UClass* Class) override;
    virtual bool FactoryCanImport(const FString& Filename) override;
    //~ End UFactory Interface

    //~ Begin FReimportHandler Interface
    virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
    virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Obj) override;
    virtual int32 GetPriority() const override;
    //~ End FReimportHandler Interface
};
