#include "GaussianSplatCameraSetAsset.h"

#include "Math/RotationMatrix.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace GaussianSplatCameraSetAssetPrivate
{
    static constexpr double SourceToUEUnitScale = 100.0;

    static FVector ConvertSourcePositionToUELocal(const FVector& SourcePositionMeters)
    {
        return FVector(
            SourcePositionMeters.Z * SourceToUEUnitScale,
            SourcePositionMeters.X * SourceToUEUnitScale,
            -SourcePositionMeters.Y * SourceToUEUnitScale);
    }

    static FVector ConvertSourceDirectionToUE(const FVector& SourceDirection)
    {
        return FVector(SourceDirection.Z, SourceDirection.X, -SourceDirection.Y);
    }

    static bool TryReadVector3(const TArray<TSharedPtr<FJsonValue>>& Values, FVector& OutVector)
    {
        if (Values.Num() != 3 || !Values[0].IsValid() || !Values[1].IsValid() || !Values[2].IsValid())
        {
            return false;
        }

        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!Values[0]->TryGetNumber(X) || !Values[1]->TryGetNumber(Y) || !Values[2]->TryGetNumber(Z))
        {
            return false;
        }

        OutVector = FVector(X, Y, Z);
        return true;
    }

    static bool TryReadRotationRows(const TArray<TSharedPtr<FJsonValue>>& RotationRows, double OutRows[3][3])
    {
        if (RotationRows.Num() != 3)
        {
            return false;
        }

        for (int32 RowIndex = 0; RowIndex < 3; ++RowIndex)
        {
            const TArray<TSharedPtr<FJsonValue>>* RowValues = nullptr;
            if (!RotationRows[RowIndex].IsValid() || !RotationRows[RowIndex]->TryGetArray(RowValues) || !RowValues || RowValues->Num() != 3)
            {
                return false;
            }

            for (int32 ColIndex = 0; ColIndex < 3; ++ColIndex)
            {
                double Value = 0.0;
                if (!(*RowValues)[ColIndex].IsValid() || !(*RowValues)[ColIndex]->TryGetNumber(Value))
                {
                    return false;
                }
                OutRows[RowIndex][ColIndex] = Value;
            }
        }

        return true;
    }

    static float ComputeFOVDegrees(int32 PixelCount, float FocalLength)
    {
        if (PixelCount <= 0 || FocalLength <= KINDA_SMALL_NUMBER)
        {
            return 90.0f;
        }

        const double FovRadians = 2.0 * FMath::Atan(static_cast<double>(PixelCount) / (2.0 * static_cast<double>(FocalLength)));
        return static_cast<float>(FMath::RadiansToDegrees(FovRadians));
    }
}

bool UGaussianSplatCameraSetAsset::LoadFromCamerasJson(const FString& FilePath)
{
    FString FileContents;
    if (!FFileHelper::LoadFileToString(FileContents, *FilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("GaussianSplatCameraSetAsset: Failed to open cameras.json file: %s"), *FilePath);
        return false;
    }

    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContents);
    TArray<TSharedPtr<FJsonValue>> RootArray;
    if (!FJsonSerializer::Deserialize(Reader, RootArray))
    {
        UE_LOG(LogTemp, Error, TEXT("GaussianSplatCameraSetAsset: Failed to parse JSON in %s"), *FilePath);
        return false;
    }

    Cameras.Reset();
    Cameras.Reserve(RootArray.Num());

    for (int32 EntryIndex = 0; EntryIndex < RootArray.Num(); ++EntryIndex)
    {
        const TSharedPtr<FJsonObject>* CameraObject = nullptr;
        if (!RootArray[EntryIndex].IsValid() || !RootArray[EntryIndex]->TryGetObject(CameraObject) || !CameraObject || !CameraObject->IsValid())
        {
            UE_LOG(LogTemp, Warning, TEXT("GaussianSplatCameraSetAsset: Skipping non-object camera entry %d in %s"), EntryIndex, *FilePath);
            continue;
        }

        FGaussianSplatImportedCamera ImportedCamera;

        double IdValue = static_cast<double>(EntryIndex);
        (*CameraObject)->TryGetNumberField(TEXT("id"), IdValue);
        ImportedCamera.Id = static_cast<int32>(FMath::RoundToInt(IdValue));

        (*CameraObject)->TryGetStringField(TEXT("img_name"), ImportedCamera.ImageName);

        double WidthValue = 0.0;
        double HeightValue = 0.0;
        double FxValue = 0.0;
        double FyValue = 0.0;
        (*CameraObject)->TryGetNumberField(TEXT("width"), WidthValue);
        (*CameraObject)->TryGetNumberField(TEXT("height"), HeightValue);
        (*CameraObject)->TryGetNumberField(TEXT("fx"), FxValue);
        (*CameraObject)->TryGetNumberField(TEXT("fy"), FyValue);

        ImportedCamera.Width = static_cast<int32>(FMath::RoundToInt(WidthValue));
        ImportedCamera.Height = static_cast<int32>(FMath::RoundToInt(HeightValue));
        ImportedCamera.Fx = static_cast<float>(FxValue);
        ImportedCamera.Fy = static_cast<float>(FyValue);
        ImportedCamera.AspectRatio = ImportedCamera.Height > 0
            ? static_cast<float>(static_cast<double>(ImportedCamera.Width) / static_cast<double>(ImportedCamera.Height))
            : 1.0f;
        ImportedCamera.HorizontalFOVDegrees =
            GaussianSplatCameraSetAssetPrivate::ComputeFOVDegrees(ImportedCamera.Width, ImportedCamera.Fx);
        ImportedCamera.VerticalFOVDegrees =
            GaussianSplatCameraSetAssetPrivate::ComputeFOVDegrees(ImportedCamera.Height, ImportedCamera.Fy);

        const TArray<TSharedPtr<FJsonValue>>* PositionValues = nullptr;
        if (!(*CameraObject)->TryGetArrayField(TEXT("position"), PositionValues) || !PositionValues)
        {
            UE_LOG(LogTemp, Warning, TEXT("GaussianSplatCameraSetAsset: Camera %d is missing a valid position array"), ImportedCamera.Id);
            continue;
        }

        FVector SourcePositionMeters = FVector::ZeroVector;
        if (!GaussianSplatCameraSetAssetPrivate::TryReadVector3(*PositionValues, SourcePositionMeters))
        {
            UE_LOG(LogTemp, Warning, TEXT("GaussianSplatCameraSetAsset: Camera %d has an invalid position array"), ImportedCamera.Id);
            continue;
        }

        const TArray<TSharedPtr<FJsonValue>>* RotationRows = nullptr;
        if (!(*CameraObject)->TryGetArrayField(TEXT("rotation"), RotationRows) || !RotationRows)
        {
            UE_LOG(LogTemp, Warning, TEXT("GaussianSplatCameraSetAsset: Camera %d is missing a valid rotation array"), ImportedCamera.Id);
            continue;
        }

        double SourceRotationRows[3][3] = {};
        if (!GaussianSplatCameraSetAssetPrivate::TryReadRotationRows(*RotationRows, SourceRotationRows))
        {
            UE_LOG(LogTemp, Warning, TEXT("GaussianSplatCameraSetAsset: Camera %d has an invalid 3x3 rotation matrix"), ImportedCamera.Id);
            continue;
        }

        // cameras.json stores the camera-to-world rotation matrix.
        // Its columns correspond to the source-space camera basis:
        //   col0 = camera right, col1 = camera down, col2 = camera forward.
        const FVector SourceRightWorld(
            SourceRotationRows[0][0],
            SourceRotationRows[1][0],
            SourceRotationRows[2][0]);
        const FVector SourceDownWorld(
            SourceRotationRows[0][1],
            SourceRotationRows[1][1],
            SourceRotationRows[2][1]);
        const FVector SourceForwardWorld(
            SourceRotationRows[0][2],
            SourceRotationRows[1][2],
            SourceRotationRows[2][2]);

        // Convert the original 3DGS camera basis into the plugin's UE basis.
        //
        // cameras.json stores a camera-to-world rotation, so its columns are the
        // camera's world-space basis vectors in the source coordinate system:
        //   col0 = Right, col1 = Down, col2 = Forward.
        //
        // This plugin uses the same 3DGS->UE axis mapping as the splat geometry:
        //   UE = ( Z, X, -Y )
        // meaning:
        //   Source Right(X)   -> UE Right(Y)
        //   Source Down(Y)    -> UE -Up(Z)
        //   Source Forward(Z) -> UE Forward(X)
        //
        // Therefore:
        //   - the camera forward vector is obtained by mapping SourceForwardWorld
        //     into UE space;
        //   - the camera up vector is the NEGATION of the mapped "down" vector,
        //     because UE expects +Z to point upward;
        //   - FRotationMatrix::MakeFromXZ() then reconstructs a UE camera rotation
        //     from X=Forward and Z=Up, matching UE's camera convention.
        const FVector UEForward =
            GaussianSplatCameraSetAssetPrivate::ConvertSourceDirectionToUE(SourceForwardWorld).GetSafeNormal();
        const FVector UEUpHint =
            (-GaussianSplatCameraSetAssetPrivate::ConvertSourceDirectionToUE(SourceDownWorld)).GetSafeNormal();

        if (UEForward.IsNearlyZero() || UEUpHint.IsNearlyZero())
        {
            UE_LOG(LogTemp, Warning, TEXT("GaussianSplatCameraSetAsset: Camera %d has a degenerate rotation basis"), ImportedCamera.Id);
            continue;
        }

        // Position uses the exact same source->UE axis mapping and meter->centimeter
        // scale as the splat data, so imported cameras stay spatially aligned with
        // the Gaussian actor in local space.
        ImportedCamera.LocalUEPosition =
            GaussianSplatCameraSetAssetPrivate::ConvertSourcePositionToUELocal(SourcePositionMeters);
        ImportedCamera.LocalUERotation = FRotationMatrix::MakeFromXZ(UEForward, UEUpHint).Rotator();

        ImportedCamera.CameraOptionName = ImportedCamera.ImageName.IsEmpty()
            ? FName(*FString::Printf(TEXT("Camera_%03d"), ImportedCamera.Id))
            : FName(*FString::Printf(TEXT("%03d_%s"), ImportedCamera.Id, *ImportedCamera.ImageName));

        Cameras.Add(MoveTemp(ImportedCamera));
    }

    SourceJsonPath = FilePath;
    CameraCount = Cameras.Num();

    if (CameraCount <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GaussianSplatCameraSetAsset: No valid cameras were imported from %s"), *FilePath);
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("GaussianSplatCameraSetAsset: Loaded %d cameras from %s"), CameraCount, *FilePath);
    return true;
}

TArray<FName> UGaussianSplatCameraSetAsset::GetCameraOptionNames() const
{
    TArray<FName> OptionNames;
    OptionNames.Reserve(Cameras.Num());
    for (const FGaussianSplatImportedCamera& Camera : Cameras)
    {
        OptionNames.Add(Camera.CameraOptionName);
    }
    return OptionNames;
}

const FGaussianSplatImportedCamera* UGaussianSplatCameraSetAsset::FindCameraByOptionName(FName CameraOptionName) const
{
    for (const FGaussianSplatImportedCamera& Camera : Cameras)
    {
        if (Camera.CameraOptionName == CameraOptionName)
        {
            return &Camera;
        }
    }

    return nullptr;
}

const FGaussianSplatImportedCamera* UGaussianSplatCameraSetAsset::GetFirstCamera() const
{
    return Cameras.IsEmpty() ? nullptr : &Cameras[0];
}
