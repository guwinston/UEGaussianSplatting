#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class GAUSSIANSPLATTING_API FGaussianSplattingModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    /** Called after GEngine is fully initialized; safe to create ViewExtension here. */
    void OnPostEngineInit();
    /** Called before engine shutdown so the view extension and sorter thread stop early. */
    void OnEnginePreExit();

    FDelegateHandle PostEngineInitHandle;
    FDelegateHandle EnginePreExitHandle;
};
