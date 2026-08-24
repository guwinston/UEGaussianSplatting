// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class GaussianSplatting : ModuleRules
{
    public GaussianSplatting(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        // Public dependencies available to other modules
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Json",
            "JsonUtilities",
            "RenderCore",
            "RHI",
        });

        // Private dependencies (only used internally)
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",       // IPluginManager
            "RHI",
            "RenderCore",     // AddShaderSourceDirectoryMapping, shader utilities
            "Renderer",       // FSceneViewExtensionBase, FPrimitiveSceneProxy, etc.
            "GeometryCore",   // UE::Geometry::TConvexHull3 (editor convex hull generation)
            "PhysicsCore",    // UBodySetup, FKConvexElem, FKAggregateGeom
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
        }

        // Access to renderer internals (scene proxies, view extensions, post process inputs)
        PrivateIncludePaths.AddRange(new string[]
        {
            Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"),
            Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"),
        });

        // Register the Android UPL so the generated manifest does not request
        // deprecated storage permissions during Development startup.
        if (Target.Platform == UnrealTargetPlatform.Android)
        {
            AdditionalPropertiesForReceipt.Add(
                "AndroidPlugin",
                Path.Combine(ModuleDirectory, "GaussianSplattingRenderer_UPL_Android.xml"));
        }

        // Enable C++20 for std::atomic and threading primitives used in GaussianSplatSorter
        // (UE's FRunnable, FEvent already handle cross-platform threading)
        bUseUnity = false;  // Disable unity build to ensure all files are compiled individually
    }
}
