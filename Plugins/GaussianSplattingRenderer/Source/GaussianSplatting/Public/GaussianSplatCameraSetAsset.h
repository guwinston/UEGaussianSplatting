#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "GaussianSplatCameraSetAsset.generated.h"

/**
 * One imported camera entry from an original 3DGS cameras.json file.
 * The local transform is stored in the same UE-local space convention as the splat asset,
 * so applying the owning actor transform keeps cameras aligned with the rendered model.
 */
USTRUCT(BlueprintType)
struct FGaussianSplatImportedCamera
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    int32 Id = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    FString ImageName;

    /** Friendly dropdown label shown on the actor. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    FName CameraOptionName;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    int32 Width = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    int32 Height = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    float Fx = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    float Fy = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    float AspectRatio = 1.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    float HorizontalFOVDegrees = 90.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    float VerticalFOVDegrees = 60.0f;

    /** Camera center converted into the plugin's UE-local asset space (centimeters). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    FVector LocalUEPosition = FVector::ZeroVector;

    /** Camera orientation converted into the plugin's UE-local asset space. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    FRotator LocalUERotation = FRotator::ZeroRotator;
};

/**
 * Imported camera set produced from the original 3DGS cameras.json output.
 */
UCLASS(BlueprintType)
class GAUSSIANSPLATTING_API UGaussianSplatCameraSetAsset : public UObject
{
    GENERATED_BODY()

public:
    /** Original source file used for import/reimport. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    FString SourceJsonPath;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    int32 CameraCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting Cameras")
    TArray<FGaussianSplatImportedCamera> Cameras;

    /** Load and convert an original 3DGS cameras.json file. */
    UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting Cameras")
    bool LoadFromCamerasJson(const FString& FilePath);

    /** Dropdown options used by AGaussianSplatActor. */
    TArray<FName> GetCameraOptionNames() const;

    /** Find a camera entry by its dropdown label. */
    const FGaussianSplatImportedCamera* FindCameraByOptionName(FName CameraOptionName) const;

    /** First imported camera, if any. */
    const FGaussianSplatImportedCamera* GetFirstCamera() const;
};
