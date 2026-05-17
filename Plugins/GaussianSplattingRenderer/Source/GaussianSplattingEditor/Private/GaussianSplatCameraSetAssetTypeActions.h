#pragma once

#include "AssetTypeActions_Base.h"
#include "GaussianSplatCameraSetAsset.h"

class FGaussianSplatCameraSetAssetTypeActions : public FAssetTypeActions_Base
{
public:
    explicit FGaussianSplatCameraSetAssetTypeActions(EAssetTypeCategories::Type InAssetCategory)
        : AssetCategory(InAssetCategory)
    {}

    virtual FText GetName() const override
    {
        return NSLOCTEXT("GaussianSplatting", "CameraSetAssetTypeName", "Gaussian Splat Camera Set");
    }

    virtual FColor GetTypeColor() const override
    {
        return FColor(234, 168, 50);
    }

    virtual UClass* GetSupportedClass() const override
    {
        return UGaussianSplatCameraSetAsset::StaticClass();
    }

    virtual uint32 GetCategories() override
    {
        return AssetCategory;
    }

    virtual bool HasActions(const TArray<UObject*>& InObjects) const override { return false; }

private:
    EAssetTypeCategories::Type AssetCategory;
};
