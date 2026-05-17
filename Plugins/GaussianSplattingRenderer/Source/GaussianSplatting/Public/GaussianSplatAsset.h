#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "GaussianSplatAsset.generated.h"

class UBodySetup;
class UStaticMesh;

/**
 * Temporary import-side 3D Gaussian Splatting data.
 * The runtime renderer uses FGaussianSplatCompressedData instead.
 */
struct FGaussianSplatData
{
	// 3D center position
	TArray<float> Positions;      // N*3 floats (x, y, z)

	// Base color (SH degree 0, DC component) + opacity
	// Color is in linear space, pre-sigmoid applied to opacity
	TArray<float> Colors;         // N*4 floats (r, g, b, a/opacity after sigmoid)
	
	// Spherical harmonics coefficients (degrees 1-3)
	// DC component is stored in Colors
	// Layout: for each splat, 45 floats (15 per RGB channel, 3+5+7 = 15 per degree)
	TArray<float> SphericalHarmonics; // N*45 floats (for degree 3), or N*9 (deg1), N*24 (deg2)

	// Estimated UE-space normal for each splat, kept for future normal-aware extensions.
	TArray<float> Normals; // N*3 floats

	// Temporary import-side data kept only long enough to build the compressed runtime payload.
	// scale = exp(LogScales) in UE units (cm), RotationQuats stores UE-space WXYZ quaternions.
	TArray<float> LogScales;     // N*3 floats
	TArray<float> RotationQuats; // N*4 floats

	// Raw data for GPU upload (interleaved per-splat)
	int32 SplatCount = 0;
	int32 SHDegree   = 0;      // 0, 1, 2, or 3
};

struct FGaussianSplatCompressedData
{
	static constexpr uint32 ChunkSize = 256u;
	static constexpr int32 ScaleDummyValueCount = 1;
	static constexpr int32 SHCodebookSize = 256;

	TArray<uint16> PackedPositions; // 16/16/16 chunk-local normalized position (x/y/z)
	TArray<uint32> PackedColors;    // 8/8/8/8: RGB quantized against per-asset min/max + alpha
	TArray<uint32> PackedRotations; // smallest-three quaternion, 2 + 10 + 10 + 10 bits
	TArray<uint32> PackedScales;    // 8/8/8 fixed log-scale quantization, SPZ-style
	TArray<uint32> PackedNormals;   // octahedral normal, low 16 bits used for now
	TArray<uint32> PackedSHData;    // direct 8-bit higher-order SH data, 4 scalar-codebook indices per uint32
	TArray<float> ScaleCodebook;    // dummy float payload kept for legacy SRV binding compatibility
	TArray<float> SHCodebook;       // shared scalar codebook for direct higher-order SH values

	TArray<FVector4f> ChunkPositionMins;
	TArray<FVector4f> ChunkPositionMaxs;

	FVector3f BoundsMin = FVector3f::ZeroVector;
	FVector3f BoundsMax = FVector3f::ZeroVector;
	FVector3f ColorQuantMin = FVector3f::ZeroVector;
	FVector3f ColorQuantMax = FVector3f(1.0f, 1.0f, 1.0f);

	int32 SplatCount = 0;
	int32 SHDegree = 0;
	int32 SHCoefficientsPerChannel = 0;
	int32 SHPackedWordsPerSplat = 0;

	bool IsValid() const
	{
		const bool bHasCoreStreams =
			SplatCount > 0
			&& PackedPositions.Num() == SplatCount * 3
			&& PackedColors.Num() == SplatCount
			&& PackedRotations.Num() == SplatCount
			&& PackedScales.Num() == SplatCount
			&& PackedNormals.Num() == SplatCount;

		if (!bHasCoreStreams)
		{
			return false;
		}

		if (ScaleCodebook.Num() != ScaleDummyValueCount)
		{
			return false;
		}

		const int32 ExpectedChunkCount = GetChunkCount();
		if (ChunkPositionMins.Num() != ExpectedChunkCount
			|| ChunkPositionMaxs.Num() != ExpectedChunkCount)
		{
			return false;
		}

		const FVector3f ColorExtent = ColorQuantMax - ColorQuantMin;
		if (ColorExtent.X <= 0.0f || ColorExtent.Y <= 0.0f || ColorExtent.Z <= 0.0f)
		{
			return false;
		}

		if (SHCoefficientsPerChannel <= 0)
		{
			return PackedSHData.IsEmpty();
		}

		return SHPackedWordsPerSplat > 0
			&& SHCodebook.Num() == SHCodebookSize
			&& PackedSHData.Num() == SplatCount * SHPackedWordsPerSplat;
	}

	int32 GetChunkCount() const
	{
		return FMath::DivideAndRoundUp(SplatCount, static_cast<int32>(ChunkSize));
	}
};

struct FGaussianSplatConvexHull
{
	TArray<FVector3f> Vertices;
	TArray<uint32> Indices;
};

/**
 * UObject asset that holds 3D Gaussian Splatting data
 * Can be loaded from a .ply file
 */
UCLASS(BlueprintType)
class GAUSSIANSPLATTING_API UGaussianSplatAsset : public UObject
{
	GENERATED_BODY()

public:
	UGaussianSplatAsset();

	/** Number of Gaussian splats */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 SplatCount = 0;

	/** Maximum spherical harmonics degree (0=constant color, 1-3=view-dependent) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting")
	int32 SHDegree = 3;

	/** Path to the source .ply file */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting", meta=(FilePathFilter="ply"))
	FString SourcePlyPath;

	/** Optional shared mesh proxy used by all instances of this asset for shadow casting and editor selection. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Selection Shape")
	TObjectPtr<UStaticMesh> ShadowProxyMesh;

	/** Use a voxel shell mesh for editor hit testing; falls back to convex decomposition if disabled or generation fails. */
	UPROPERTY(EditAnywhere, Category = "Gaussian Splatting|Selection Shape")
	bool bUseVoxelSelectionShell = true;

	/** Target number of voxels along the shell mesh max dimension. Higher values improve detail but cost more on import/rebuild. */
	UPROPERTY(EditAnywhere, Category = "Gaussian Splatting|Selection Shape", meta=(ClampMin="24", ClampMax="256"))
	int32 SelectionShellTargetResolution = 96;

	/** Multiplier applied to the estimated point spacing when building the density field radius. */
	UPROPERTY(EditAnywhere, Category = "Gaussian Splatting|Selection Shape", meta=(ClampMin="1.0", ClampMax="8.0"))
	float SelectionShellInfluenceRadiusScale = 2.5f;

	/** Isosurface threshold used to extract the voxel shell from the density field. */
	UPROPERTY(EditAnywhere, Category = "Gaussian Splatting|Selection Shape", meta=(ClampMin="0.01", ClampMax="2.0"))
	float SelectionShellDensityThreshold = 0.18f;

	/** Maximum number of seed points fed into marching cubes continuation mode. */
	UPROPERTY(EditAnywhere, Category = "Gaussian Splatting|Selection Shape", meta=(ClampMin="128", ClampMax="16384"))
	int32 SelectionShellMaxSeedPoints = 4096;

	/** Upper bound on the number of convex hulls generated for the collision fallback. */
	UPROPERTY(EditAnywhere, Category = "Gaussian Splatting|Selection Shape", meta=(ClampMin="1", ClampMax="128"))
	int32 MaxSelectionConvexHulls = 64;

	/** Compressed render payload kept serialized so runtime no longer expands splat data into float arrays. */
	UPROPERTY(NonTransactional)
	TArray<uint16> SerializedPackedPositions;

	UPROPERTY(NonTransactional)
	TArray<uint32> SerializedPackedColors;

	UPROPERTY(NonTransactional)
	TArray<uint32> SerializedPackedRotations;

	UPROPERTY(NonTransactional)
	TArray<uint32> SerializedPackedScales;

	UPROPERTY(NonTransactional)
	TArray<uint32> SerializedPackedNormals;

	UPROPERTY(NonTransactional)
	TArray<float> SerializedScaleCodebook;

	UPROPERTY(NonTransactional)
	TArray<float> SerializedSHCodebook;

	UPROPERTY(NonTransactional)
	TArray<uint32> SerializedPackedSHData;

	UPROPERTY(NonTransactional)
	TArray<FVector4f> SerializedChunkPositionMins;

	UPROPERTY(NonTransactional)
	TArray<FVector4f> SerializedChunkPositionMaxs;

	UPROPERTY(NonTransactional)
	FVector3f SerializedBoundsMin = FVector3f::ZeroVector;

	UPROPERTY(NonTransactional)
	FVector3f SerializedBoundsMax = FVector3f::ZeroVector;

	UPROPERTY(NonTransactional)
	FVector3f SerializedColorQuantMin = FVector3f::ZeroVector;

	UPROPERTY(NonTransactional)
	FVector3f SerializedColorQuantMax = FVector3f(1.0f, 1.0f, 1.0f);

	UPROPERTY(NonTransactional)
	int32 SerializedSHCoefficientsPerChannel = 0;

	UPROPERTY(NonTransactional)
	int32 SerializedSHPackedWordsPerSplat = 0;

	UPROPERTY(NonTransactional)
	int32 SerializedCompressedDataVersion = 0;

	/** Runtime cache rebuilt from the serialized arrays after the asset is loaded. */
	TSharedPtr<FGaussianSplatData> SplatData;
	TSharedPtr<FGaussianSplatCompressedData> CompressedSplatData;

	/** Load from a .ply file */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	bool LoadFromPly(const FString& FilePath);

	/** Check if data is loaded */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	bool IsLoaded() const { return CompressedSplatData.IsValid() && CompressedSplatData->IsValid(); }

	/** Get bounding box of all splat centers */
	FBox GetBounds() const;

	UBodySetup* GetSelectionBodySetup() const;

	virtual void PostLoad() override;

	void RebuildSelectionGeometry();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Convex decomposition generated from the splat centers for editor selection/collision. */
	TArray<FGaussianSplatConvexHull> ConvexHulls;
	/** Merged render mesh built from either the voxel shell or convex fallback for hit testing and shadow casting. */
	UPROPERTY(NonTransactional)
	TArray<FVector3f> SelectionMeshVertices;
	/** Triangle indices into SelectionMeshVertices (3 indices per triangle). */
	UPROPERTY(NonTransactional)
	TArray<uint32> SelectionMeshIndices;

	/** Shared collision body built from ConvexHulls so components do not rebuild it on every placement. */
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> SelectionBodySetup;

private:
	bool BuildCompressedDataFromRuntimeData();
	void CacheSerializedCompressedDataFromRuntimeData();
	bool RestoreCompressedRuntimeDataFromSerializedState();
	bool HasSerializedCompressedData() const;
};
