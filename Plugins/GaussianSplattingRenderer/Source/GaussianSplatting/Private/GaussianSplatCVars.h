#pragma once

#include "CoreMinimal.h"

namespace GaussianSplatCVars
{
    int32 GetRenderModeOnRenderThread();
    int32 GetRasterModeOnRenderThread();
    int32 GetEnableHigherOrderSHOnRenderThread();
    int32 GetUseNoAAProjectionOnRenderThread();
    int32 GetEnableAntialiasingOnRenderThread();

    int32 GetCullModeOnAnyThread();
    float GetSplatFrustumSlackOnAnyThread();
}
