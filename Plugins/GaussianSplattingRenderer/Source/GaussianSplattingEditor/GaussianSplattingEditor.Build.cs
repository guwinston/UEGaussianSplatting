// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GaussianSplattingEditor : ModuleRules
{
    public GaussianSplattingEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "GaussianSplatting",  // access UGaussianSplatAsset, UGaussianSplatComponent
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UnrealEd",           // UFactory, AssetImportTask, FReimportHandler
            "AssetTools",         // IAssetTools, FAssetTypeActions_Base
            "ContentBrowser",     // drag-drop integration
            "PropertyEditor",     // IDetailCustomization
            "Projects",           // IPluginManager
            "Slate",
            "SlateCore",
            "EditorStyle",
            "InputCore",
        });
    }
}
