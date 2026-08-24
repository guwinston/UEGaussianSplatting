#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameters.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterMacros.h"
#include "RHICommandList.h"
#include "ShaderPermutation.h"
#include "SceneView.h"

// ============================================================
//  GPU Object Cull Compute Shader
//  Evaluates one visibility flag per object from its local bounds.
// ============================================================
class GAUSSIANSPLATTING_API FGaussianObjectCullCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianObjectCullCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianObjectCullCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, PerObjectBuffer)
        SHADER_PARAMETER(int32, ObjectCount)
        SHADER_PARAMETER(int32, CullMode)
        SHADER_PARAMETER(FMatrix44f, WorldToView)
        SHADER_PARAMETER(FMatrix44f, ViewToClip)
        SHADER_PARAMETER_UAV(RWBuffer<uint32>, OutObjectVisibility)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), 64);
    }
};

// ============================================================
//  GPU Splat Cull + Depth Key Build Compute Shader
//
//  Runs over the merged global splat stream and writes:
//    - one sort key per global splat
//    - one visible-splat counter for the indirect draw path
//
//  The value stream sorted by SortGPUBuffers is the identity global-splat index.
// ============================================================
class GAUSSIANSPLATTING_API FGaussianBuildSortKeysCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianBuildSortKeysCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianBuildSortKeysCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(Buffer<uint>, GlobalPackedPositionBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, GlobalPackedColorBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, GlobalPackedScaleBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GlobalChunkPositionMinBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GlobalChunkPositionMaxBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, PerObjectBuffer)
        SHADER_PARAMETER_SRV(Buffer<uint32>, GlobalObjectIndexBuffer)
        SHADER_PARAMETER_SRV(Buffer<uint32>, ObjectVisibilityBuffer)
        SHADER_PARAMETER(int32, TotalSplatCount)
        SHADER_PARAMETER(uint32, SplatDispatchOffset)
        SHADER_PARAMETER(int32, CullMode)
        SHADER_PARAMETER(float, TanHalfFovX)
        SHADER_PARAMETER(float, TanHalfFovY)
        SHADER_PARAMETER(float, FrustumSlack)
        SHADER_PARAMETER(uint32, EnableScreenSizeCull)
        SHADER_PARAMETER(float, ScreenSizeCullMinPixels)
        SHADER_PARAMETER(float, MaxFocalLengthPixels)
        SHADER_PARAMETER(FMatrix44f, WorldToView)
        SHADER_PARAMETER_UAV(RWBuffer<uint32>, OutDepthKeys)
        SHADER_PARAMETER_UAV(RWBuffer<uint32>, OutVisibleCount)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), 64);
    }
};

// ============================================================
//  GPU Draw-Indirect Args Build Compute Shader
//  Converts the visible-splat counter into a non-indexed draw args buffer.
// ============================================================
class GAUSSIANSPLATTING_API FGaussianBuildIndirectArgsCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianBuildIndirectArgsCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianBuildIndirectArgsCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(Buffer<uint32>, VisibleCountBuffer)
        SHADER_PARAMETER_UAV(RWBuffer<uint32>, OutDrawIndirectArgs)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), 1);
    }
};

// ============================================================
//  3DGS Shader Permutation Dimensions
// ============================================================

class FGaussianSplatRasterModeDim : SHADER_PERMUTATION_INT("RASTER_MODE", 2);

// ============================================================
//  Per-Object GPU Descriptor
//
//  Stored in a StructuredBuffer<FGaussianSplatObjectGPUDesc> on the GPU.
//  Indexed by the object index resolved from GlobalObjectIndexBuffer.
//
//  Layout must match the HLSL struct in GaussianSplat.usf and
//  GaussianSplatCullAndSortKeyGen.usf.
// ============================================================
struct FGaussianSplatObjectGPUDesc
{
    FMatrix44f LocalToWorld;
    FMatrix44f WorldToLocal;

    uint32 SplatOffset = 0;
    uint32 SplatCount = 0;
    uint32 ChunkOffset = 0;
    uint32 SHDataOffset = 0;

    uint32 MaxSHDegree = 0;
    uint32 SHDegree = 0;
    uint32 SHCoefficientsPerChannel = 0;
    uint32 SHPackedWordsPerSplat = 0;

    float SplatScale = 1.0f;
    float AlphaCullThreshold = 0.004f;
    float _Pad0 = 0.0f;
    float ColorQuantMinX = 0.0f;

    FVector3f LocalBoundsMin = FVector3f::ZeroVector;
    float ColorQuantMinY = 0.0f;

    FVector3f LocalBoundsMax = FVector3f::ZeroVector;
    float ColorQuantMinZ = 0.0f;

    FVector4f _Pad1 = FVector4f::Zero();

    FVector3f ColorQuantMax = FVector3f(1.0f, 1.0f, 1.0f);
    uint32 ScaleCodebookOffset = 0;

    uint32 SHCodebookOffset = 0;
    uint32 _Pad6 = 0;
    uint32 _Pad7 = 0;
    uint32 _Pad8 = 0;
};
static_assert(sizeof(FGaussianSplatObjectGPUDesc) % 16 == 0,
    "FGaussianSplatObjectGPUDesc must be 16-byte aligned for GPU structured buffer layout.");

// ============================================================
//  3DGS Merged / Global-Sort Vertex Shader
//
//  All active GaussianSplat proxies are rendered in a single draw call.
//  The draw path uses:
//    - SortedVisibleIndexBuffer : sorted global splat indices
//    - GlobalObjectIndexBuffer  : global splat index -> object index
//
//  Global merged buffers hold concatenated packed data for all objects:
//    - GlobalPackedPositionBuffer   : N_total*3 R16_UINT chunk-local position components
//    - GlobalPackedColorBuffer      : N_total packed uint32 colors/opacity
//    - GlobalPackedRotationBuffer   : N_total packed uint32 quaternions
//    - GlobalPackedScaleBuffer      : N_total packed uint32 fixed log-scales
//    - GlobalPackedSHDataBuffer     : direct 8-bit higher-order SH words
//    - GlobalChunkPosition*Buffer   : per-chunk position min/max metadata
// ============================================================
class GAUSSIANSPLATTING_API FGaussianSplatVS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianSplatVS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatVS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GlobalPackedPositionBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GlobalPackedColorBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GlobalPackedRotationBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GlobalPackedScaleBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GlobalPackedSHDataBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, GlobalSHCodebookBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GlobalChunkPositionMinBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GlobalChunkPositionMaxBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GlobalObjectIndexBuffer)

        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, PerObjectBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortedVisibleIndexBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VisibleCountBuffer)

        SHADER_PARAMETER(FMatrix44f, WorldToView)
        SHADER_PARAMETER(FMatrix44f, ViewToClip)
        SHADER_PARAMETER(FVector3f, CameraPosition)
        SHADER_PARAMETER(FVector2f, FocalLength)
        SHADER_PARAMETER(FVector2f, ViewportMin)
        SHADER_PARAMETER(FVector2f, ViewportSize)

        SHADER_PARAMETER(int32, TotalSplatCount)
        SHADER_PARAMETER(int32, ObjectCount)
        SHADER_PARAMETER(uint32, EnableAntialiasing)
        SHADER_PARAMETER(uint32, EnableOpacityAwareBounds)
        SHADER_PARAMETER(float, PreExposure)
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
    END_SHADER_PARAMETER_STRUCT()

    using FPermutationDomain = TShaderPermutationDomain<FGaussianSplatRasterModeDim>;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        // This desktop VS reads several buffer SRVs. Android Mobile Vulkan
        // explicitly disables vertex-shader SRVs.
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }
};

// ============================================================
//  3DGS Direct Mesh Shader
//  Projects each Gaussian once and emits its four quad vertices plus two triangles.
// ============================================================
class GAUSSIANSPLATTING_API FGaussianSplatMS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianSplatMS);
    using FParameters = FGaussianSplatVS::FParameters;
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatMS, FGlobalShader);

    using FPermutationDomain = FGaussianSplatVS::FPermutationDomain;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return RHISupportsMeshShadersTier0(Parameters.Platform);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }
};

// ============================================================
//  3DGS Direct Pixel Shader
// ============================================================
class GAUSSIANSPLATTING_API FGaussianSplatPS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianSplatPS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
        SHADER_PARAMETER(FMatrix44f, WorldToView)
        SHADER_PARAMETER(FVector2f, FocalLength)
        SHADER_PARAMETER(FVector2f, ViewportMin)
        SHADER_PARAMETER(FVector2f, ViewportSize)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
        SHADER_PARAMETER(FVector2f, SceneDepthViewportMin)
        SHADER_PARAMETER(FVector2f, SceneDepthViewportSize)
        SHADER_PARAMETER(FVector2f, SceneDepthTextureExtentInverse)
        SHADER_PARAMETER(uint32, UseManualSceneDepthTest)
    END_SHADER_PARAMETER_STRUCT()

    using FPermutationDomain = TShaderPermutationDomain<FGaussianSplatRasterModeDim>;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }
};

// ============================================================
//  Fullscreen composite shaders
// ============================================================
class GAUSSIANSPLATTING_API FGaussianSplatCompositeVS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianSplatCompositeVS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCompositeVS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
    }
};

class GAUSSIANSPLATTING_API FGaussianSplatCompositePS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianSplatCompositePS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCompositePS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GaussianAccumTexture)
        SHADER_PARAMETER(float, PreExposure)
        SHADER_PARAMETER(uint32, ConvertOutputToLinear)
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
    END_SHADER_PARAMETER_STRUCT()

    using FPermutationDomain = TShaderPermutationDomain<>;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
    }
};

// ============================================================
//  Combined pass parameters
// ============================================================
BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSplatPassParameters, )
    SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatVS::FParameters, VS)
    SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatPS::FParameters, PS)
    RDG_BUFFER_ACCESS(DrawIndirectArgsBuffer, ERHIAccess::IndirectArgs)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSplatCompositePassParameters, )
    SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatCompositePS::FParameters, PS)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()
