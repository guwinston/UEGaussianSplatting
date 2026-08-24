#pragma once

#include "CoreMinimal.h"

namespace GaussianSplatCVars
{
    int32 GetRenderModeOnRenderThread();
    int32 GetRasterModeOnRenderThread();
    int32 GetEnableHigherOrderSHOnRenderThread();
    int32 GetUseNoAAProjectionOnRenderThread();
    int32 GetEnableAntialiasingOnRenderThread();
    int32 GetOpacityAwareBoundsOnRenderThread();
    /** Geometry backend: 0 = VS+PS, 1 = MS+PS (falls back to VS when unsupported). */
    int32 GetGeometryModeOnRenderThread();

    int32 GetCullModeOnAnyThread();
    int32 GetForceSortEveryFrameOnAnyThread();
    float GetSplatFrustumSlackOnAnyThread();
    int32 GetScreenSizeCullOnAnyThread();
    float GetScreenSizeCullMinPixelsOnAnyThread();

    /** GPU sort/render method: 0 = UE built-in, 1 = DeviceRadix8, 2 = stochastic no-sort. */
    int32 GetSortMethodOnRenderThread();
    /** Number of stochastic frames to accumulate. 0 disables temporal accumulation. */
    int32 GetStochasticTemporalSamplesOnRenderThread();
    int32 GetDeviceRadixPassCountOnRenderThread();
    int32 GetDeviceRadixWriteFinalKeysOnRenderThread();
}
