#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GaussianSplatAsset.h"
#include "PhysicsEngine/BodySetup.h"
#include "GaussianSplatComponent.generated.h"

class FGaussianSplatSceneProxy;

/**
 * Scene Component that renders a 3D Gaussian Splatting model
 * Place this in a level to visualize .ply Gaussian splat data
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent), HideCategories=(Object, Activation, "Components|Activation", Physics, Collision, Lighting))
class GAUSSIANSPLATTING_API UGaussianSplatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UGaussianSplatComponent();

	//~ Begin UPrimitiveComponent Interface (Collision / Editor Selection)
	virtual UBodySetup* GetBodySetup() override;
#if WITH_EDITOR
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
#endif
	//~ End UPrimitiveComponent Interface

	/** The Gaussian splat asset to render */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting")
	TObjectPtr<UGaussianSplatAsset> GaussianSplatAsset;

	/** Scale multiplier for splat sizes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting", meta=(ClampMin="0.01", ClampMax="10.0"))
	float SplatScale = 1.0f;

	/** Maximum SH degree to use for view-dependent color (0=constant, 1-3=view-dependent) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting", meta=(ClampMin="0", ClampMax="3"))
	int32 MaxSHDegree = 3;

	/** Minimum opacity threshold - splats below this are culled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting", meta=(ClampMin="0.0", ClampMax="1.0"))
	float AlphaCullThreshold = 0.004f;

	/** Load a splat asset from a file path at runtime */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	void LoadFromFile(const FString& FilePath);

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	//~ End UPrimitiveComponent Interface

	/** Rebuild BodySetup convex hull from the current asset (called when asset changes). */
	void RebuildBodySetup();

	//~ Begin USceneComponent Interface
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	//~ End USceneComponent Interface

	//~ Begin UActorComponent Interface
	virtual void OnRegister() override;
	//~ End UActorComponent Interface

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	
	/** Called when the splat asset changes */
	void OnSplatAssetChanged();

private:
	/** Collision/selection body setup holding the convex hull. Rebuilt when the asset changes. */
	UPROPERTY()
	TObjectPtr<UBodySetup> BodySetup;

	friend class FGaussianSplatSceneProxy;
};
