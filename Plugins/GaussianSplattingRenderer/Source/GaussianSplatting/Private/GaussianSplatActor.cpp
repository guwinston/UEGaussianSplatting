#include "GaussianSplatActor.h"
#include "GaussianSplatCameraSetAsset.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Misc/ScopedSlowTask.h"
#include "UnrealClient.h"
#endif

namespace GaussianSplatActorPrivate
{
#if WITH_EDITOR
static FEditorViewportClient* FindBestEditorViewportClient()
{
    if (!GEditor)
    {
        return nullptr;
    }

    FViewport* ActiveViewport = GEditor->GetActiveViewport();
    // Prefer the currently active perspective viewport, but fall back to any
    // perspective editor viewport so snapping still works when focus is elsewhere.
    for (FEditorViewportClient* ViewportClient : GEditor->GetAllViewportClients())
    {
        if (ViewportClient && ViewportClient->IsPerspective() && ActiveViewport && ViewportClient->Viewport == ActiveViewport)
        {
            return ViewportClient;
        }
    }

    for (FEditorViewportClient* ViewportClient : GEditor->GetAllViewportClients())
    {
        if (ViewportClient && ViewportClient->IsPerspective())
        {
            return ViewportClient;
        }
    }

    return nullptr;
}

static FString ResolveCameraRenderOutputDirectory(const AGaussianSplatActor& Actor)
{
    FString OutputDirectory = Actor.CameraRenderOutputDirectory;
    if (OutputDirectory.IsEmpty())
    {
        OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GaussianSplatCameraRenders"));
    }

    OutputDirectory = FPaths::ConvertRelativePathToFull(OutputDirectory);
    FPaths::NormalizeFilename(OutputDirectory);
    return OutputDirectory;
}

static void ApplyImportedCameraToViewport(
    const AGaussianSplatActor& Actor,
    const FGaussianSplatImportedCamera& Camera,
    FEditorViewportClient& ViewportClient,
    bool bApplyFOV)
{
    const FTransform ActorTransform = Actor.GetActorTransform();
    const FVector WorldLocation = ActorTransform.TransformPosition(Camera.LocalUEPosition);
    const FQuat WorldRotation = ActorTransform.TransformRotation(Camera.LocalUERotation.Quaternion());

    ViewportClient.SetViewLocation(WorldLocation);
    ViewportClient.SetViewRotation(WorldRotation.Rotator());

    if (bApplyFOV)
    {
        ViewportClient.FOVAngle = Camera.HorizontalFOVDegrees;
        ViewportClient.ViewFOV = Camera.HorizontalFOVDegrees;
    }

    ViewportClient.Invalidate();
}

static void ApplyInterpolatedImportedCameraToViewport(
    const AGaussianSplatActor& Actor,
    const FGaussianSplatImportedCamera& FromCamera,
    const FGaussianSplatImportedCamera& ToCamera,
    float Alpha,
    FEditorViewportClient& ViewportClient,
    bool bApplyFOV)
{
    const float ClampedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
    const FTransform ActorTransform = Actor.GetActorTransform();

    const FVector WorldLocation = FMath::Lerp(
        ActorTransform.TransformPosition(FromCamera.LocalUEPosition),
        ActorTransform.TransformPosition(ToCamera.LocalUEPosition),
        ClampedAlpha);

    const FQuat WorldRotation = FQuat::Slerp(
        ActorTransform.TransformRotation(FromCamera.LocalUERotation.Quaternion()),
        ActorTransform.TransformRotation(ToCamera.LocalUERotation.Quaternion()),
        ClampedAlpha).GetNormalized();

    ViewportClient.SetViewLocation(WorldLocation);
    ViewportClient.SetViewRotation(WorldRotation.Rotator());

    if (bApplyFOV)
    {
        const float FOV = FMath::Lerp(FromCamera.HorizontalFOVDegrees, ToCamera.HorizontalFOVDegrees, ClampedAlpha);
        ViewportClient.FOVAngle = FOV;
        ViewportClient.ViewFOV = FOV;
    }

    ViewportClient.Invalidate();
}

static bool SaveViewportToPng(FEditorViewportClient& ViewportClient, const FString& FilePath)
{
    FViewport* Viewport = ViewportClient.Viewport;
    if (!Viewport)
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Editor viewport is not available for capture."));
        return false;
    }

    Viewport->Draw(false);
    FlushRenderingCommands();

    TArray<FColor> Pixels;
    if (!Viewport->ReadPixels(Pixels) || Pixels.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Failed to read pixels from editor viewport."));
        return false;
    }

    const FIntPoint ViewportSize = Viewport->GetSizeXY();
    if (ViewportSize.X <= 0 || ViewportSize.Y <= 0 || Pixels.Num() != ViewportSize.X * ViewportSize.Y)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("GaussianSplatActor: Invalid viewport capture size %dx%d with %d pixels."),
            ViewportSize.X,
            ViewportSize.Y,
            Pixels.Num());
        return false;
    }

    for (FColor& Pixel : Pixels)
    {
        Pixel.A = 255;
    }

    TArray64<uint8> CompressedPng;
    FImageUtils::PNGCompressImageArray(ViewportSize.X, ViewportSize.Y, MakeArrayView(Pixels), CompressedPng);
    if (CompressedPng.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Failed to compress viewport capture as PNG."));
        return false;
    }

    if (!FFileHelper::SaveArrayToFile(CompressedPng, *FilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Failed to save viewport capture: %s"), *FilePath);
        return false;
    }

    return true;
}
#endif
}

AGaussianSplatActor::AGaussianSplatActor()
{
    PrimaryActorTick.bCanEverTick = true;

    GaussianSplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplatComponent"));
    RootComponent = GaussianSplatComponent;
}

void AGaussianSplatActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

#if WITH_EDITOR
    TickImportedCameraPlayback(DeltaSeconds);
#endif
}

void AGaussianSplatActor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();

    ApplyCollisionProxySetting();
}

void AGaussianSplatActor::BeginPlay()
{
    Super::BeginPlay();

    ApplyCollisionProxySetting();

    // Prefer the assigned/imported asset. SourcePlyPath is only a runtime fallback
    // for transient actors that were never given a serialized GaussianSplatAsset.
    const bool bHasLoadedAsset = GaussianSplatComponent
        && GaussianSplatComponent->GaussianSplatAsset
        && GaussianSplatComponent->GaussianSplatAsset->IsLoaded();
    if (!bHasLoadedAsset && !SourcePlyPath.IsEmpty())
    {
        ReloadPly();
    }
}

bool AGaussianSplatActor::ShouldTickIfViewportsOnly() const
{
    return true;
}

void AGaussianSplatActor::ReloadPly()
{
    if (SourcePlyPath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: SourcePlyPath is empty, nothing to load."));
        return;
    }

    // Normalize path separators
    FString NormalizedPath = FPaths::ConvertRelativePathToFull(SourcePlyPath);
    FPaths::NormalizeFilename(NormalizedPath);

    if (!IFileManager::Get().FileExists(*NormalizedPath))
    {
        UE_LOG(LogTemp, Error,
            TEXT("GaussianSplatActor: .ply file not found: %s"), *NormalizedPath);
        return;
    }

    if (GaussianSplatComponent)
    {
        UE_LOG(LogTemp, Log,
            TEXT("GaussianSplatActor: Loading PLY: %s"), *NormalizedPath);
        GaussianSplatComponent->LoadFromFile(NormalizedPath);
    }
}

void AGaussianSplatActor::LoadFromFile(const FString& FilePath)
{
    SourcePlyPath = FilePath;
    ReloadPly();
}

void AGaussianSplatActor::SetGaussianSplatAsset(UGaussianSplatAsset* InAsset)
{
    if (!GaussianSplatComponent)
    {
        return;
    }

    GaussianSplatComponent->GaussianSplatAsset = InAsset;
    if (InAsset)
    {
        SourcePlyPath = InAsset->SourcePlyPath;
    }

    GaussianSplatComponent->OnSplatAssetChanged();
}

TArray<FName> AGaussianSplatActor::GetImportedCameraOptions() const
{
    return ImportedCameraSet ? ImportedCameraSet->GetCameraOptionNames() : TArray<FName>{};
}

void AGaussianSplatActor::ApplyCollisionProxySetting()
{
    if (!GaussianSplatComponent)
    {
        return;
    }

    GaussianSplatComponent->SetCollisionEnabled(
        bEnableCollisionProxy ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
    GaussianSplatComponent->SetCollisionResponseToAllChannels(
        bEnableCollisionProxy ? ECR_Block : ECR_Ignore);
    GaussianSplatComponent->SetCanEverAffectNavigation(bEnableCollisionProxy);
}

void AGaussianSplatActor::SnapActiveViewportToSelectedCamera()
{
#if WITH_EDITOR
    StopImportedCameraPlayback();
    EnsureSelectedImportedCameraIsValid();

    const FGaussianSplatImportedCamera* SelectedCamera = FindSelectedImportedCamera();
    if (!SelectedCamera)
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: No imported camera is available to snap to."));
        return;
    }

    FEditorViewportClient* ViewportClient = GaussianSplatActorPrivate::FindBestEditorViewportClient();
    if (!ViewportClient)
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Could not find an active perspective editor viewport."));
        return;
    }

    GaussianSplatActorPrivate::ApplyImportedCameraToViewport(
        *this,
        *SelectedCamera,
        *ViewportClient,
        bApplySelectedCameraFOVToEditorViewport);

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatActor: Snapped active viewport to imported camera %s"),
        *SelectedCamera->CameraOptionName.ToString());
#else
    UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Imported camera viewport snapping is only available in the editor."));
#endif
}

#if WITH_EDITOR
void AGaussianSplatActor::CaptureActiveViewportImage()
{
    StopImportedCameraPlayback();
    FEditorViewportClient* ViewportClient = GaussianSplatActorPrivate::FindBestEditorViewportClient();
    if (!ViewportClient)
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Could not find an active perspective editor viewport."));
        return;
    }

    const FString OutputDirectory = GaussianSplatActorPrivate::ResolveCameraRenderOutputDirectory(*this);
    if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Failed to create output directory: %s"), *OutputDirectory);
        return;
    }

    const FString FileName = FString::Printf(
        TEXT("viewport_%s.png"),
        *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
    const FString FilePath = FPaths::Combine(OutputDirectory, FileName);

    if (GaussianSplatActorPrivate::SaveViewportToPng(*ViewportClient, FilePath))
    {
        UE_LOG(LogTemp, Log, TEXT("GaussianSplatActor: Saved viewport capture to %s"), *FilePath);
    }
}

void AGaussianSplatActor::RenderAllImportedCameraImages()
{
    StopImportedCameraPlayback();
    if (!ImportedCameraSet || ImportedCameraSet->Cameras.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: No imported cameras are available to render."));
        return;
    }

    FEditorViewportClient* ViewportClient = GaussianSplatActorPrivate::FindBestEditorViewportClient();
    if (!ViewportClient)
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Could not find an active perspective editor viewport."));
        return;
    }

    const FString OutputDirectory = GaussianSplatActorPrivate::ResolveCameraRenderOutputDirectory(*this);
    if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Failed to create output directory: %s"), *OutputDirectory);
        return;
    }

    const FVector OriginalViewLocation = ViewportClient->GetViewLocation();
    const FRotator OriginalViewRotation = ViewportClient->GetViewRotation();
    const float OriginalFOVAngle = ViewportClient->FOVAngle;
    const float OriginalViewFOV = ViewportClient->ViewFOV;

    int32 SavedCount = 0;
    FScopedSlowTask RenderSlowTask(
        static_cast<float>(ImportedCameraSet->Cameras.Num()),
        NSLOCTEXT("GaussianSplatActor", "RenderImportedCameraImages", "Rendering imported camera images..."));
    RenderSlowTask.MakeDialog(true);

    for (const FGaussianSplatImportedCamera& Camera : ImportedCameraSet->Cameras)
    {
        if (RenderSlowTask.ShouldCancel())
        {
            break;
        }

        const FText CameraLabel = Camera.ImageName.IsEmpty()
            ? FText::Format(
                NSLOCTEXT("GaussianSplatActor", "RenderImportedCameraById", "Rendering camera {0}"),
                FText::AsNumber(Camera.Id))
            : FText::Format(
                NSLOCTEXT("GaussianSplatActor", "RenderImportedCameraByName", "Rendering {0}"),
                FText::FromString(Camera.ImageName));
        RenderSlowTask.EnterProgressFrame(1.0f, CameraLabel);

        GaussianSplatActorPrivate::ApplyImportedCameraToViewport(
            *this,
            Camera,
            *ViewportClient,
            bApplySelectedCameraFOVToEditorViewport);

        FString BaseName = Camera.ImageName.IsEmpty()
            ? FString::Printf(TEXT("camera_%03d"), Camera.Id)
            : FPaths::GetBaseFilename(Camera.ImageName);
        BaseName = FPaths::MakeValidFileName(BaseName);

        const FString FileName = FString::Printf(TEXT("%03d_%s.png"), Camera.Id, *BaseName);
        const FString FilePath = FPaths::Combine(OutputDirectory, FileName);
        if (GaussianSplatActorPrivate::SaveViewportToPng(*ViewportClient, FilePath))
        {
            ++SavedCount;
        }
    }

    ViewportClient->SetViewLocation(OriginalViewLocation);
    ViewportClient->SetViewRotation(OriginalViewRotation);
    ViewportClient->FOVAngle = OriginalFOVAngle;
    ViewportClient->ViewFOV = OriginalViewFOV;
    ViewportClient->Invalidate();

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatActor: Saved %d/%d imported camera captures to %s"),
        SavedCount,
        ImportedCameraSet->Cameras.Num(),
        *OutputDirectory);
}

void AGaussianSplatActor::StartImportedCameraPlayback()
{
    EnsureSelectedImportedCameraIsValid();

    if (!ImportedCameraSet || ImportedCameraSet->Cameras.Num() < 2)
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: At least two imported cameras are required for viewport playback."));
        return;
    }

    FEditorViewportClient* ViewportClient = GaussianSplatActorPrivate::FindBestEditorViewportClient();
    if (!ViewportClient)
    {
        UE_LOG(LogTemp, Warning, TEXT("GaussianSplatActor: Could not find an active perspective editor viewport."));
        return;
    }

    int32 StartCameraIndex = 0;
    if (SelectedImportedCamera != NAME_None)
    {
        StartCameraIndex = ImportedCameraSet->Cameras.IndexOfByPredicate([this](const FGaussianSplatImportedCamera& Camera)
        {
            return Camera.CameraOptionName == SelectedImportedCamera;
        });
        if (StartCameraIndex == INDEX_NONE)
        {
            StartCameraIndex = 0;
        }
    }

    CurrentPlaybackCameraIndex = StartCameraIndex;
    CurrentPlaybackSegmentAlpha = 0.0f;
    bImportedCameraPlaybackActive = true;

    GaussianSplatActorPrivate::ApplyImportedCameraToViewport(
        *this,
        ImportedCameraSet->Cameras[CurrentPlaybackCameraIndex],
        *ViewportClient,
        bApplySelectedCameraFOVToEditorViewport);

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatActor: Started imported camera playback from %s."),
        *ImportedCameraSet->Cameras[CurrentPlaybackCameraIndex].CameraOptionName.ToString());
}

void AGaussianSplatActor::StopImportedCameraPlayback()
{
    if (!bImportedCameraPlaybackActive)
    {
        return;
    }

    bImportedCameraPlaybackActive = false;
    CurrentPlaybackSegmentAlpha = 0.0f;

    UE_LOG(LogTemp, Log, TEXT("GaussianSplatActor: Stopped imported camera playback."));
}
#endif

void AGaussianSplatActor::TickImportedCameraPlayback(float DeltaSeconds)
{
#if WITH_EDITOR
    if (!bImportedCameraPlaybackActive || !ImportedCameraSet || ImportedCameraSet->Cameras.Num() < 2)
    {
        return;
    }

    FEditorViewportClient* ViewportClient = GaussianSplatActorPrivate::FindBestEditorViewportClient();
    if (!ViewportClient)
    {
        return;
    }

    const int32 CameraCount = ImportedCameraSet->Cameras.Num();
    int32 NextCameraIndex = CurrentPlaybackCameraIndex + 1;
    if (NextCameraIndex >= CameraCount)
    {
        if (!bLoopImportedCameraPlayback)
        {
            GaussianSplatActorPrivate::ApplyImportedCameraToViewport(
                *this,
                ImportedCameraSet->Cameras.Last(),
                *ViewportClient,
                bApplySelectedCameraFOVToEditorViewport);
            StopImportedCameraPlayback();
            return;
        }

        NextCameraIndex = 0;
    }

    const float SegmentSeconds = FMath::Max(0.05f, CameraPlaybackSecondsPerSegment);
    CurrentPlaybackSegmentAlpha += DeltaSeconds / SegmentSeconds;

    const float SmoothedAlpha = FMath::InterpEaseInOut(0.0f, 1.0f, FMath::Clamp(CurrentPlaybackSegmentAlpha, 0.0f, 1.0f), 2.0f);
    GaussianSplatActorPrivate::ApplyInterpolatedImportedCameraToViewport(
        *this,
        ImportedCameraSet->Cameras[CurrentPlaybackCameraIndex],
        ImportedCameraSet->Cameras[NextCameraIndex],
        SmoothedAlpha,
        *ViewportClient,
        bApplySelectedCameraFOVToEditorViewport);

    if (CurrentPlaybackSegmentAlpha >= 1.0f)
    {
        CurrentPlaybackCameraIndex = NextCameraIndex;
        CurrentPlaybackSegmentAlpha = 0.0f;
        SelectedImportedCamera = ImportedCameraSet->Cameras[CurrentPlaybackCameraIndex].CameraOptionName;
    }
#else
    (void)DeltaSeconds;
#endif
}

const FGaussianSplatImportedCamera* AGaussianSplatActor::FindSelectedImportedCamera() const
{
    if (!ImportedCameraSet)
    {
        return nullptr;
    }

    if (SelectedImportedCamera != NAME_None)
    {
        if (const FGaussianSplatImportedCamera* SelectedCamera = ImportedCameraSet->FindCameraByOptionName(SelectedImportedCamera))
        {
            return SelectedCamera;
        }
    }

    return ImportedCameraSet->GetFirstCamera();
}

void AGaussianSplatActor::EnsureSelectedImportedCameraIsValid()
{
    // No camera set means there is nothing valid to keep selected.
    if (!ImportedCameraSet)
    {
        SelectedImportedCamera = NAME_None;
        return;
    }

    // Keep the current selection if it still exists in the imported camera set.
    if (SelectedImportedCamera != NAME_None && ImportedCameraSet->FindCameraByOptionName(SelectedImportedCamera))
    {
        return;
    }

    // Otherwise fall back to the first available imported camera, or clear the
    // selection if the camera set is unexpectedly empty.
    const FGaussianSplatImportedCamera* FirstCamera = ImportedCameraSet->GetFirstCamera();
    SelectedImportedCamera = FirstCamera ? FirstCamera->CameraOptionName : NAME_None;
}

#if WITH_EDITOR
void AGaussianSplatActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    // Reload whenever SourcePlyPath is changed in the Details panel
    if (!IsActorBeingDestroyed()
        && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive
        && PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SourcePlyPath))
    {
        if (!SourcePlyPath.IsEmpty())
        {
            ReloadPly();
        }
    }
    else if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, ImportedCameraSet))
    {
        EnsureSelectedImportedCameraIsValid();
    }
    else if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, bEnableCollisionProxy))
    {
        ApplyCollisionProxySetting();
    }
}
#endif
