#include "GaussianSplattingModule.h"
#include "GaussianSplatViewExtension.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "ShaderCore.h"  // AddShaderSourceDirectoryMapping â€?part of RenderCore

#define LOCTEXT_NAMESPACE "FGaussianSplattingModule"

void FGaussianSplattingModule::StartupModule()
{
    // Register the shader virtual path so USF files under Plugins/GaussianSplattingRenderer/Shaders/
    // can be included with /Plugin/GaussianSplattingRenderer/...
    const FString PluginBaseDir = IPluginManager::Get().FindPlugin(TEXT("GaussianSplattingRenderer"))->GetBaseDir();
    const FString PluginShaderDir = FPaths::Combine(PluginBaseDir, TEXT("Shaders"));
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/GaussianSplattingRenderer"), PluginShaderDir);

    UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: Plugin started. Shader dir: %s"), *PluginShaderDir);

    EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(
        this, &FGaussianSplattingModule::OnEnginePreExit);

    // Delay ViewExtension creation until after GEngine is fully initialized.
    // FSceneViewExtensions::NewExtension() calls GEngine->ViewExtensions which
    // requires GEngine != nullptr. At StartupModule time GEngine may not yet exist
    // (loading phase "Default" runs before engine init completes).
    if (GEngine)
    {
        // Already initialized (e.g., hot-reload scenario)
        FGaussianSplatViewExtension::Create();
    }
    else
    {
        PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
            this, &FGaussianSplattingModule::OnPostEngineInit);
    }
}

void FGaussianSplattingModule::OnPostEngineInit()
{
    // GEngine is now valid â€?safe to register the view extension
    FGaussianSplatViewExtension::Create();

    // Unregister so we don't get called again
    FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
    PostEngineInitHandle.Reset();

    UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: ViewExtension created (post-engine-init)."));
}

void FGaussianSplattingModule::OnEnginePreExit()
{
    FGaussianSplatViewExtension::Destroy();
}

void FGaussianSplattingModule::ShutdownModule()
{
    // Clean up the delegate in case shutdown happens before engine init
    if (PostEngineInitHandle.IsValid())
    {
        FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
        PostEngineInitHandle.Reset();
    }

    if (EnginePreExitHandle.IsValid())
    {
        FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);
        EnginePreExitHandle.Reset();
    }

    FGaussianSplatViewExtension::Destroy();
    UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: Plugin shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSplattingModule, GaussianSplatting)
