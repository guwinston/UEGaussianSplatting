#include "GaussianSplatActorDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "PropertyHandle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GaussianSplatActorDetails"

TSharedRef<IDetailCustomization> FGaussianSplatActorDetails::MakeInstance()
{
    return MakeShared<FGaussianSplatActorDetails>();
}

void FGaussianSplatActorDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
    // Resolve the actor instance currently being edited in the Details panel.
    // We only need the first AGaussianSplatActor because the custom button acts on
    // the selected actor directly.
    TArray<TWeakObjectPtr<UObject>> EditedObjects;
    DetailBuilder.GetObjectsBeingCustomized(EditedObjects);

    AGaussianSplatActor* EditedActor = nullptr;
    for (const TWeakObjectPtr<UObject>& EditedObject : EditedObjects)
    {
        if (AGaussianSplatActor* Actor = Cast<AGaussianSplatActor>(EditedObject.Get()))
        {
            EditedActor = Actor;
            break;
        }
    }

    // Grab handles for the camera-related properties that we want to regroup
    // under the main "Gaussian Splatting" section instead of letting UE place
    // them with its default category/function layout rules.
    TSharedRef<IPropertyHandle> ImportedCameraSetProperty =
        DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, ImportedCameraSet));
    TSharedRef<IPropertyHandle> SelectedImportedCameraProperty =
        DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SelectedImportedCamera));
    TSharedRef<IPropertyHandle> ApplyFOVProperty =
        DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, bApplySelectedCameraFOVToEditorViewport));
    TSharedRef<IPropertyHandle> CameraPlaybackSecondsProperty =
        DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, CameraPlaybackSecondsPerSegment));
    TSharedRef<IPropertyHandle> CameraPlaybackLoopProperty =
        DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, bLoopImportedCameraPlayback));
    TSharedRef<IPropertyHandle> CameraRenderOutputDirectoryProperty =
        DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, CameraRenderOutputDirectory));

    // Hide the default rows before re-adding them below. This avoids duplicate
    // camera sections in the Details panel.
    DetailBuilder.HideProperty(ImportedCameraSetProperty);
    DetailBuilder.HideProperty(SelectedImportedCameraProperty);
    DetailBuilder.HideProperty(ApplyFOVProperty);
    DetailBuilder.HideProperty(CameraPlaybackSecondsProperty);
    DetailBuilder.HideProperty(CameraPlaybackLoopProperty);
    DetailBuilder.HideProperty(CameraRenderOutputDirectoryProperty);

    // Reuse the main "Gaussian Splatting" category for core splat settings.
    IDetailCategoryBuilder& GaussianCategory =
        DetailBuilder.EditCategory(TEXT("Gaussian Splatting"), LOCTEXT("GaussianSplattingCategory", "Gaussian Splatting"));
    IDetailCategoryBuilder& ShadowProxyCategory =
        DetailBuilder.EditCategory(TEXT("Gaussian Splatting|Shadow Proxy"), LOCTEXT("GaussianSplattingShadowProxyCategory", "Shadow Proxy"));

    if (EditedActor && EditedActor->GaussianSplatComponent)
    {
        TArray<UObject*> ExternalObjects;
        ExternalObjects.Add(EditedActor->GaussianSplatComponent);
        TArray<UObject*> AssetObjects;
        if (EditedActor->GaussianSplatComponent->GaussianSplatAsset)
        {
            AssetObjects.Add(EditedActor->GaussianSplatComponent->GaussianSplatAsset);
            DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, ShadowProxyMesh), UGaussianSplatAsset::StaticClass());
        }

        // Hide the component's default rows before re-adding them into the
        // categories above, otherwise the same properties would appear twice.
        DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale), UGaussianSplatComponent::StaticClass());
        DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxSHDegree), UGaussianSplatComponent::StaticClass());
        DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, AlphaCullThreshold), UGaussianSplatComponent::StaticClass());
        DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UPrimitiveComponent, CastShadow), UPrimitiveComponent::StaticClass());

        GaussianCategory.AddExternalObjectProperty(ExternalObjects, GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale));
        GaussianCategory.AddExternalObjectProperty(ExternalObjects, GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxSHDegree));
        GaussianCategory.AddExternalObjectProperty(ExternalObjects, GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, AlphaCullThreshold));

        ShadowProxyCategory.AddExternalObjectProperty(ExternalObjects, GET_MEMBER_NAME_CHECKED(UPrimitiveComponent, CastShadow));
        if (AssetObjects.Num() > 0)
        {
            ShadowProxyCategory.AddExternalObjectProperty(AssetObjects, GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, ShadowProxyMesh));
        }
    }

    IDetailGroup& CameraGroup =
        GaussianCategory.AddGroup(TEXT("GaussianSplattingCameras"), LOCTEXT("ImportedCamerasGroup", "Imported Cameras"));

    // Reinsert the three camera properties in the exact order we want.
    CameraGroup.AddPropertyRow(ImportedCameraSetProperty);
    CameraGroup.AddPropertyRow(SelectedImportedCameraProperty);
    CameraGroup.AddPropertyRow(ApplyFOVProperty);
    CameraGroup.AddPropertyRow(CameraPlaybackSecondsProperty);
    CameraGroup.AddPropertyRow(CameraPlaybackLoopProperty);
    CameraGroup.AddPropertyRow(CameraRenderOutputDirectoryProperty);

    // Add a custom action row for snapping the active editor viewport to the
    // currently selected imported camera. We build this manually because the
    // default CallInEditor button path caused category duplication.
    CameraGroup.AddWidgetRow()
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("SnapViewportLabel", "Viewport"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MinDesiredWidth(260.0f)
        [
            SNew(SButton)
            .Text(LOCTEXT("SnapViewportButton", "Snap Active Viewport To Selected Camera"))
            .IsEnabled_Lambda([EditedActor]()
            {
                return EditedActor != nullptr;
            })
            .OnClicked_Lambda([EditedActor]()
            {
                if (EditedActor)
                {
                    EditedActor->SnapActiveViewportToSelectedCamera();
                }
                return FReply::Handled();
            })
        ];

    CameraGroup.AddWidgetRow()
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("CaptureViewportLabel", "Capture"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MinDesiredWidth(260.0f)
        [
            SNew(SButton)
            .Text(LOCTEXT("CaptureViewportButton", "Save Current Viewport Image"))
            .IsEnabled_Lambda([EditedActor]()
            {
                return EditedActor != nullptr;
            })
            .OnClicked_Lambda([EditedActor]()
            {
                if (EditedActor)
                {
                    EditedActor->CaptureActiveViewportImage();
                }
                return FReply::Handled();
            })
        ];

    CameraGroup.AddWidgetRow()
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("PlaybackLabel", "Playback"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MinDesiredWidth(260.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("PlaybackStartButton", "Play Camera Tour"))
                .IsEnabled_Lambda([EditedActor]()
                {
                    return EditedActor != nullptr && EditedActor->ImportedCameraSet != nullptr;
                })
                .OnClicked_Lambda([EditedActor]()
                {
                    if (EditedActor)
                    {
                        EditedActor->StartImportedCameraPlayback();
                    }
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("PlaybackStopButton", "Stop"))
                .IsEnabled_Lambda([EditedActor]()
                {
                    return EditedActor != nullptr;
                })
                .OnClicked_Lambda([EditedActor]()
                {
                    if (EditedActor)
                    {
                        EditedActor->StopImportedCameraPlayback();
                    }
                    return FReply::Handled();
                })
            ]
        ];

    CameraGroup.AddWidgetRow()
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("RenderAllCamerasLabel", "Batch Render"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MinDesiredWidth(260.0f)
        [
            SNew(SButton)
            .Text(LOCTEXT("RenderAllCamerasButton", "Render All Imported Camera Images"))
            .IsEnabled_Lambda([EditedActor]()
            {
                return EditedActor != nullptr && EditedActor->ImportedCameraSet != nullptr;
            })
            .OnClicked_Lambda([EditedActor]()
            {
                if (EditedActor)
                {
                    EditedActor->RenderAllImportedCameraImages();
                }
                return FReply::Handled();
            })
        ];
}

#undef LOCTEXT_NAMESPACE
