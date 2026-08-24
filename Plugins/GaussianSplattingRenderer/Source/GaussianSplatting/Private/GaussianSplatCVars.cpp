#include "GaussianSplatCVars.h"

static TAutoConsoleVariable<int32> CVarGaussianSplatRasterMode(
    TEXT("r.GaussianSplat.RasterMode"),
    0,
    TEXT("3DGS raster mode:\n")
    TEXT(" 0 = unit-circle pixel evaluation\n")
    TEXT(" 1 = conic / CUDA-like pixel evaluation (intended to be equivalent)"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatRenderMode(
    TEXT("r.GaussianSplat.RenderMode"),
    1,
    TEXT("Global 3DGS render mode for the current frame.\n")
    TEXT(" 0 = Integrate With UE\n")
    TEXT(" 1 = Match Original 3DGS\n")
    TEXT("All Gaussian splat components use the same render mode each frame."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatEnableHigherOrderSH(
    TEXT("r.GaussianSplat.EnableHigherOrderSH"),
    1,
    TEXT("Enable higher-order spherical harmonics for Gaussian splats.\n")
    TEXT(" 0 = disabled, use base color only\n")
    TEXT(" 1 = enabled"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatUseNoAAProjection(
    TEXT("r.GaussianSplat.UseNoAAProjection"),
    1,
    TEXT("Use an unjittered projection matrix for Gaussian splats rendered after TAA/TSR.\n")
    TEXT(" 0 = use the view's jittered projection\n")
    TEXT(" 1 = remove TAA/TSR jitter from the 3DGS projection"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatEnableAntialiasing(
    TEXT("r.GaussianSplat.EnableAntialiasing"),
    0,
    TEXT("Enable Mip-Splatting style opacity compensation for the 2D covariance low-pass filter.\n")
    TEXT(" 0 = disabled, preserve existing opacity behavior\n")
    TEXT(" 1 = enabled, scale opacity by sqrt(det(original covariance) / det(filtered covariance))"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatSortMethod(
    TEXT("r.GaussianSplat.SortMethod"),
    1,
    TEXT("GPU sort algorithm for Gaussian splat depth ordering.\n")
    TEXT(" 0 = UE built-in SortGPUBuffers (default)\n")
    TEXT(" 1 = DeviceRadixSort (configurable 8-bit LSD passes)\n")
    TEXT(" 2 = stochastic splat (compact visible IDs, skip sorting; experimental)"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatStochasticTemporalSamples(
    TEXT("r.GaussianSplat.StochasticTemporalSamples"),
    1000,
    TEXT("Number of independent stochastic frames accumulated per stable view (0-4096).\n")
    TEXT(" 0 = disable accumulation and display the raw noisy sample\n")
    TEXT(" N = running-average N samples, then freeze until the view or scene changes"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatDeviceRadixPasses(
    TEXT("r.GaussianSplat.DeviceRadixPasses"),
    4,
    TEXT("Number of 8-bit DeviceRadixSort passes (1-4).\n")
    TEXT("Passes below 4 sort the most-significant key bytes and trade depth precision for speed."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatDeviceRadixWriteFinalKeys(
    TEXT("r.GaussianSplat.DeviceRadixWriteFinalKeys"),
    0,
    TEXT("Write DeviceRadixSort's final key stream.\n")
    TEXT(" 0 = values only (faster; sufficient for Gaussian rendering)\n")
    TEXT(" 1 = also preserve the final sorted keys"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatCullMode(
    TEXT("r.GaussianSplat.CullMode"),
    2,
    TEXT("Gaussian splat GPU cull mode before sorting.\n")
    TEXT(" 0 = disabled (no filtering at all, AfterCull == BeforeCull)\n")
    TEXT(" 1 = object-level frustum + alpha threshold + behind-camera reject\n")
    TEXT(" 2 = 1 + per-splat XY frustum cull"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatFrustumSlack(
    TEXT("r.GaussianSplat.SplatFrustumSlack"),
    1.3f,
    TEXT("Frustum expansion factor for per-splat GPU cull.\n")
    TEXT("Values > 1 expand the cull frustum to keep border splats.\n")
    TEXT("1.0 = exact mathematical frustum. Recommended range: 1.05 - 1.5."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatScreenSizeCull(
    TEXT("r.GaussianSplat.ScreenSizeCull"),
    1,
    TEXT("Cull splats whose conservative projected diameter is below the pixel threshold.\n")
    TEXT("Requires CullMode >= 1. Disabled by default for an unchanged baseline."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatScreenSizeCullMinPixels(
    TEXT("r.GaussianSplat.ScreenSizeCullMinPixels"),
    1.0f,
    TEXT("Minimum conservative projected splat diameter in pixels when ScreenSizeCull is enabled."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatOpacityAwareBounds(
    TEXT("r.GaussianSplat.OpacityAwareBounds"),
    1,
    TEXT("Shrink the rasterized ellipse bounds using opacity and the 1/255 pixel cutoff.\n")
    TEXT(" 0 = fixed sqrt(8)-sigma bounds\n")
    TEXT(" 1 = opacity-aware bounds, capped at the existing sqrt(8)-sigma extent"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatForceSortEveryFrame(
    TEXT("r.GaussianSplat.ForceSortEveryFrame"),
    0,
    TEXT("Force the GPU cull and sort pipeline to run every frame for profiling.\n")
    TEXT(" 0 = reuse the previous sort while the view is unchanged\n")
    TEXT(" 1 = run cull, key generation, sort and indirect-args generation every frame"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatGeometryMode(
    TEXT("r.GaussianSplat.GeometryMode"),
    1,
    TEXT("Select the Gaussian geometry backend.\n")
    TEXT(" 0 = original VS + PS\n")
    TEXT(" 1 = mesh shader + PS (falls back to VS when unsupported)"),
    ECVF_RenderThreadSafe);


namespace GaussianSplatCVars
{
    int32 GetRenderModeOnRenderThread()
    {
        return FMath::Clamp(CVarGaussianSplatRenderMode.GetValueOnRenderThread(), 0, 1);
    }

    int32 GetRasterModeOnRenderThread()
    {
        return FMath::Clamp(CVarGaussianSplatRasterMode.GetValueOnRenderThread(), 0, 1);
    }

    int32 GetEnableHigherOrderSHOnRenderThread()
    {
        return CVarGaussianSplatEnableHigherOrderSH.GetValueOnRenderThread() != 0 ? 1 : 0;
    }

    int32 GetUseNoAAProjectionOnRenderThread()
    {
        return CVarGaussianSplatUseNoAAProjection.GetValueOnRenderThread() != 0 ? 1 : 0;
    }

    int32 GetEnableAntialiasingOnRenderThread()
    {
        return CVarGaussianSplatEnableAntialiasing.GetValueOnRenderThread() != 0 ? 1 : 0;
    }

    int32 GetOpacityAwareBoundsOnRenderThread()
    {
        return CVarGaussianSplatOpacityAwareBounds.GetValueOnRenderThread() != 0 ? 1 : 0;
    }

    int32 GetGeometryModeOnRenderThread()
    {
        return FMath::Clamp(CVarGaussianSplatGeometryMode.GetValueOnRenderThread(), 0, 1);
    }


    int32 GetCullModeOnAnyThread()
    {
        return FMath::Clamp(CVarGaussianSplatCullMode.GetValueOnAnyThread(), 0, 2);
    }

    int32 GetForceSortEveryFrameOnAnyThread()
    {
        return CVarGaussianSplatForceSortEveryFrame.GetValueOnAnyThread() != 0 ? 1 : 0;
    }

    float GetSplatFrustumSlackOnAnyThread()
    {
        return FMath::Max(1.0f, CVarGaussianSplatFrustumSlack.GetValueOnAnyThread());
    }

    int32 GetScreenSizeCullOnAnyThread()
    {
        return CVarGaussianSplatScreenSizeCull.GetValueOnAnyThread() != 0 ? 1 : 0;
    }

    float GetScreenSizeCullMinPixelsOnAnyThread()
    {
        return FMath::Max(0.0f, CVarGaussianSplatScreenSizeCullMinPixels.GetValueOnAnyThread());
    }

    int32 GetSortMethodOnRenderThread()
    {
        return FMath::Clamp(CVarGaussianSplatSortMethod.GetValueOnRenderThread(), 0, 2);
    }

    int32 GetStochasticTemporalSamplesOnRenderThread()
    {
        return FMath::Clamp(CVarGaussianSplatStochasticTemporalSamples.GetValueOnRenderThread(), 0, 4096);
    }

    int32 GetDeviceRadixPassCountOnRenderThread()
    {
        return FMath::Clamp(CVarGaussianSplatDeviceRadixPasses.GetValueOnRenderThread(), 1, 4);
    }

    int32 GetDeviceRadixWriteFinalKeysOnRenderThread()
    {
        return CVarGaussianSplatDeviceRadixWriteFinalKeys.GetValueOnRenderThread() != 0 ? 1 : 0;
    }
}
