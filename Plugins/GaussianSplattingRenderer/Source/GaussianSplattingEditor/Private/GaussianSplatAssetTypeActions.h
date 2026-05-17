#pragma once

#include "AssetTypeActions_Base.h"
#include "GaussianSplatAsset.h"

/**
 * Registers UGaussianSplatAsset in the Content Browser with a custom category,
 * tooltip and thumbnail color.
 */
class FGaussianSplatAssetTypeActions : public FAssetTypeActions_Base
{
public:
    explicit FGaussianSplatAssetTypeActions(EAssetTypeCategories::Type InAssetCategory)
        : AssetCategory(InAssetCategory)
    {}

    // FAssetTypeActions_Base interface
    virtual FText GetName() const override
    {
        return NSLOCTEXT("GaussianSplatting", "AssetTypeName", "Gaussian Splat Asset");
    }

    virtual FColor GetTypeColor() const override
    {
        // Distinct teal color so the asset is easy to spot
        return FColor(0, 188, 188);
    }

    virtual UClass* GetSupportedClass() const override
    {
        return UGaussianSplatAsset::StaticClass();
    }

    virtual uint32 GetCategories() override
    {
        return AssetCategory;
    }

    virtual bool IsImportedAsset() const override
    {
        return true;
    }

    virtual void GetResolvedSourceFilePaths(const TArray<UObject*>& TypeAssets, TArray<FString>& OutSourceFilePaths) const override
    {
        OutSourceFilePaths.Reset();
        for (UObject* Object : TypeAssets)
        {
            if (const UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Object))
            {
                if (!Asset->SourcePlyPath.IsEmpty())
                {
                    OutSourceFilePaths.Add(Asset->SourcePlyPath);
                }
            }
        }
    }

    virtual bool HasActions(const TArray<UObject*>& InObjects) const override { return false; }

private:
    EAssetTypeCategories::Type AssetCategory;
};
