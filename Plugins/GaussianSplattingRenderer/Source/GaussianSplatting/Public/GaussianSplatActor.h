#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianSplatActor.generated.h"

class UGaussianSplatAsset;
class UGaussianSplatCameraSetAsset;
class UGaussianSplatComponent;
struct FGaussianSplatImportedCamera;

/**
 * Actor that displays a 3D Gaussian Splatting model.
 *
 * Quick start:
 *   1. Place a GaussianSplatActor in the level (or drag a .ply onto the viewport).
 *   2. In the Details panel, set "Source Ply Path" to an absolute path of your .ply file.
 *      The model will reload automatically whenever the path changes.
 *   3. Adjust SplatScale / AlphaCullThreshold on the GaussianSplatComponent as needed.
 */
UCLASS(ClassGroup=(Rendering), meta=(ShortTooltip="Renders a 3D Gaussian Splatting .ply file"))
class GAUSSIANSPLATTING_API AGaussianSplatActor : public AActor
{
    GENERATED_BODY()

public:
    AGaussianSplatActor();
    virtual void Tick(float DeltaSeconds) override;

    /** The 3DGS rendering component */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
    TObjectPtr<UGaussianSplatComponent> GaussianSplatComponent;

    /**
     * Absolute path to the .ply file to render.
     * Changing this in the Details panel will immediately reload the model.
     * Example: C:/captures/garden/point_cloud/iteration_30000/point_cloud.ply
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting",
              meta = (FilePathFilter = "ply", RelativeToGameDir = false))
    FString SourcePlyPath;

    /** Enable the generated selection/proxy body as real collision. Disabled by default so splats do not block the player. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Collision")
    bool bEnableCollisionProxy = false;

    /** Reload the model from SourcePlyPath (called automatically on property change). */
    UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
    void ReloadPly();

    /** Load a .ply file and render it (sets SourcePlyPath and reloads). */
    UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
    void LoadFromFile(const FString& FilePath);

    /** Assign an imported GaussianSplatAsset directly. */
    UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
    void SetGaussianSplatAsset(UGaussianSplatAsset* InAsset);

    /** Imported 3DGS camera set from cameras.json. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Cameras")
    TObjectPtr<UGaussianSplatCameraSetAsset> ImportedCameraSet;

    /** Camera entry used when snapping the editor viewport. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Cameras", meta = (GetOptions = "GetImportedCameraOptions"))
    FName SelectedImportedCamera;

    /** Whether snapping also applies the imported horizontal FOV to the active editor viewport. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Cameras")
    bool bApplySelectedCameraFOVToEditorViewport = true;

    /** Travel time, in seconds, between consecutive imported cameras during viewport playback. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Cameras", meta = (ClampMin = "0.05", ClampMax = "60.0"))
    float CameraPlaybackSecondsPerSegment = 1.5f;

    /** Whether camera playback loops back to the first imported camera after reaching the end. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Cameras")
    bool bLoopImportedCameraPlayback = true;

    /** Folder used by the editor camera render buttons. Empty uses Saved/GaussianSplatCameraRenders. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Cameras",
              meta = (RelativeToGameDir = false))
    FString CameraRenderOutputDirectory;

    /** Dropdown options for SelectedImportedCamera. */
    UFUNCTION(BlueprintPure, Category = "Gaussian Splatting|Cameras")
    TArray<FName> GetImportedCameraOptions() const;

    /** Move the active editor viewport to the currently selected imported camera. */
    void SnapActiveViewportToSelectedCamera();

#if WITH_EDITOR
    /** Save the active editor viewport as a PNG. */
    void CaptureActiveViewportImage();

    /** Render every imported cameras.json camera through the active editor viewport and save PNGs. */
    void RenderAllImportedCameraImages();

    /** Start smooth editor viewport playback through the imported camera sequence. */
    void StartImportedCameraPlayback();

    /** Stop smooth editor viewport playback. */
    void StopImportedCameraPlayback();
#endif

protected:
    virtual void PostRegisterAllComponents() override;
    virtual void BeginPlay() override;
    virtual bool ShouldTickIfViewportsOnly() const override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void TickImportedCameraPlayback(float DeltaSeconds);
    const FGaussianSplatImportedCamera* FindSelectedImportedCamera() const;
    void EnsureSelectedImportedCameraIsValid();
    void ApplyCollisionProxySetting();

    bool bImportedCameraPlaybackActive = false;
    int32 CurrentPlaybackCameraIndex = 0;
    float CurrentPlaybackSegmentAlpha = 0.0f;
};
