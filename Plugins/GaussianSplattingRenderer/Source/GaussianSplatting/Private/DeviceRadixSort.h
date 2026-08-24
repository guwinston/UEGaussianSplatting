// DeviceRadixSort integration. The algorithm source is MIT licensed by Thomas Smith.
#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "RenderGraphResources.h"
#include "ShaderParameterStruct.h"

class FRDGBuilder;

class FDeviceRadixUpsweepCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDeviceRadixUpsweepCS);
	SHADER_USE_PARAMETER_STRUCT(FDeviceRadixUpsweepCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, e_numKeys)
		SHADER_PARAMETER(uint32, e_radixShift)
		SHADER_PARAMETER(uint32, e_threadBlocks)
		SHADER_PARAMETER(uint32, e_passIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, b_sort)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_globalHist)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_passHist)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class FDeviceRadixScanCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDeviceRadixScanCS);
	SHADER_USE_PARAMETER_STRUCT(FDeviceRadixScanCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, e_numKeys)
		SHADER_PARAMETER(uint32, e_radixShift)
		SHADER_PARAMETER(uint32, e_threadBlocks)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_passHist)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class FDeviceRadixWriteKeysDim : SHADER_PERMUTATION_BOOL("DEVICE_RADIX_WRITE_KEYS");
class FDeviceRadixIdentityPayloadDim : SHADER_PERMUTATION_BOOL("DEVICE_RADIX_IDENTITY_PAYLOAD");
using FDeviceRadixDownsweepPermutationDomain = TShaderPermutationDomain<FDeviceRadixWriteKeysDim, FDeviceRadixIdentityPayloadDim>;

class FDeviceRadixDownsweepCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDeviceRadixDownsweepCS);
	SHADER_USE_PARAMETER_STRUCT(FDeviceRadixDownsweepCS, FGlobalShader);

public:
	using FPermutationDomain = FDeviceRadixDownsweepPermutationDomain;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, e_numKeys)
		SHADER_PARAMETER(uint32, e_radixShift)
		SHADER_PARAMETER(uint32, e_threadBlocks)
		SHADER_PARAMETER(uint32, e_passIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, b_sort)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, b_sortPayload)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_alt)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_altPayload)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_globalHist)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, b_passHist)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

namespace DeviceRadixSort
{
	constexpr uint32 PartitionSize = 3840u;
	constexpr uint32 Radix = 256u;
	constexpr uint32 MaxPassCount = 4u;

	struct FOptions
	{
		/** Number of 8-bit LSD passes. Valid range is [1, 4]. */
		uint32 PassCount = MaxPassCount;
		/** First key bit consumed by pass zero. Must be byte aligned. */
		uint32 FirstBit = 0u;
		/** The final keys are optional when the caller only consumes the sorted values. */
		bool bWriteFinalKeys = true;
		/** Generate payload 0..N-1 in pass zero instead of reading Values[0]. */
		bool bFirstPayloadIsIdentity = false;
	};

	bool IsSupported();

	/**
	 * Sorts ascending 32-bit key/value pairs using the DeviceRadixSort kernel topology.
	 * When FinalValues is provided, the last pass writes payloads there directly.
	 */
	void Enqueue(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef Keys[2],
		FRDGBufferRef Values[2],
		uint32 ElementCount,
		FRDGBufferRef FinalValues = nullptr,
		bool* bOutResultInInputBuffers = nullptr,
		const FOptions* Options = nullptr);
}
