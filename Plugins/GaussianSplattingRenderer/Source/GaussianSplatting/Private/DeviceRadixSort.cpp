// DeviceRadixSort integration. The algorithm source is MIT licensed by Thomas Smith.
#include "DeviceRadixSort.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"

IMPLEMENT_GLOBAL_SHADER(FDeviceRadixUpsweepCS, "/Plugin/GaussianSplattingRenderer/Sort/DeviceRadixSort.usf", "Upsweep", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FDeviceRadixScanCS, "/Plugin/GaussianSplattingRenderer/Sort/DeviceRadixSort.usf", "Scan", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FDeviceRadixDownsweepCS, "/Plugin/GaussianSplattingRenderer/Sort/DeviceRadixSort.usf", "Downsweep", SF_Compute);

namespace
{
	bool UsesAndroidMobileSafePath(EShaderPlatform Platform)
	{
		return IsAndroidPlatform(Platform) && IsVulkanPlatform(Platform);
	}

	bool ShouldCompileDeviceRadix(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
	}

	void ModifyDeviceRadixEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("KEY_UINT"), 1);
		OutEnvironment.SetDefine(TEXT("PAYLOAD_UINT"), 1);
		OutEnvironment.SetDefine(TEXT("SHOULD_ASCEND"), 1);
		OutEnvironment.SetDefine(TEXT("SORT_PAIRS"), 1);
		OutEnvironment.SetDefine(
			TEXT("DEVICE_RADIX_MOBILE_SAFE"),
			UsesAndroidMobileSafePath(Parameters.Platform) ? 1u : 0u);
		if (!UsesAndroidMobileSafePath(Parameters.Platform))
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
}

bool FDeviceRadixUpsweepCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return ShouldCompileDeviceRadix(Parameters);
}

void FDeviceRadixUpsweepCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	ModifyDeviceRadixEnvironment(Parameters, OutEnvironment);
}

bool FDeviceRadixScanCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return ShouldCompileDeviceRadix(Parameters);
}

void FDeviceRadixScanCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	ModifyDeviceRadixEnvironment(Parameters, OutEnvironment);
}

bool FDeviceRadixDownsweepCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return ShouldCompileDeviceRadix(Parameters);
}

void FDeviceRadixDownsweepCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	ModifyDeviceRadixEnvironment(Parameters, OutEnvironment);
}

namespace DeviceRadixSort
{
	bool IsSupported()
	{
		if (UsesAndroidMobileSafePath(GMaxRHIShaderPlatform))
		{
			return GMaxWorkGroupInvocations >= 256
				&& GMaxComputeSharedMemory >= 16 * 1024;
		}

		if (!GRHISupportsWaveOperations || !RHISupportsWaveOperations(GMaxRHIShaderPlatform))
		{
			return false;
		}

		return true;
	}

	void Enqueue(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef Keys[2],
		FRDGBufferRef Values[2],
		uint32 ElementCount,
		FRDGBufferRef FinalValues,
		bool* bOutResultInInputBuffers,
		const FOptions* Options)
	{
		check(Keys[0] && Keys[1] && Values[0] && Values[1]);
		check(ElementCount > 0u);

		const FOptions DefaultOptions;
		const FOptions& ResolvedOptions = Options ? *Options : DefaultOptions;
		checkf(ResolvedOptions.PassCount >= 1u && ResolvedOptions.PassCount <= MaxPassCount,
			TEXT("DeviceRadixSort pass count must be in [1, 4]."));
		checkf((ResolvedOptions.FirstBit & 7u) == 0u
			&& ResolvedOptions.FirstBit + ResolvedOptions.PassCount * 8u <= 32u,
			TEXT("DeviceRadixSort bit range must be byte aligned and contained in a 32-bit key."));

		const uint32 ThreadBlocks = FMath::DivideAndRoundUp(ElementCount, PartitionSize);
		const FRDGBufferDesc PassHistogramDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), ThreadBlocks * Radix);
		const FRDGBufferDesc GlobalHistogramDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), ResolvedOptions.PassCount * Radix);
		FRDGBufferRef PassHistogram = GraphBuilder.CreateBuffer(PassHistogramDesc, TEXT("DeviceRadix.PassHistogram"));
		FRDGBufferRef GlobalHistogram = GraphBuilder.CreateBuffer(GlobalHistogramDesc, TEXT("DeviceRadix.GlobalHistogram"));

		const FRDGBufferSRVRef KeySRVs[2] = {
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Keys[0], PF_R32_UINT)),
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Keys[1], PF_R32_UINT)) };
		const FRDGBufferSRVRef ValueSRVs[2] = {
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Values[0], PF_R32_UINT)),
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Values[1], PF_R32_UINT)) };
		const FRDGBufferUAVRef KeyUAVs[2] = {
			GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Keys[0], PF_R32_UINT)),
			GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Keys[1], PF_R32_UINT)) };
		const FRDGBufferUAVRef ValueUAVs[2] = {
			GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Values[0], PF_R32_UINT)),
			GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Values[1], PF_R32_UINT)) };
		const FRDGBufferUAVRef FinalValueUAV = FinalValues
			? GraphBuilder.CreateUAV(FRDGBufferUAVDesc(FinalValues, PF_R32_UINT))
			: nullptr;
		const FRDGBufferUAVRef GlobalHistogramUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(GlobalHistogram, PF_R32_UINT));
		const FRDGBufferUAVRef PassHistogramUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PassHistogram, PF_R32_UINT));

		// Let RDG select the platform clear path and clear only the histogram slices used by
		// this invocation. The old kernel always launched 1024 threads, even for fewer passes.
		AddClearUAVPass(GraphBuilder, GlobalHistogramUAV, 0u);

		const FRDGBufferUAVRef PassHistogramRW = PassHistogramUAV;
		const FRDGBufferUAVRef GlobalHistogramRW = GlobalHistogramUAV;
		for (uint32 Pass = 0u; Pass < ResolvedOptions.PassCount; ++Pass)
		{
			const uint32 SrcIndex = (Pass & 1u) == 0u ? 0u : 1u;
			const uint32 DstIndex = SrcIndex ^ 1u;
			const uint32 RadixShift = ResolvedOptions.FirstBit + Pass * 8u;
			const bool bFinalPass = Pass + 1u == ResolvedOptions.PassCount;

			{
				FDeviceRadixUpsweepCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDeviceRadixUpsweepCS::FParameters>();
				Parameters->e_numKeys = ElementCount;
				Parameters->e_radixShift = RadixShift;
				Parameters->e_threadBlocks = ThreadBlocks;
				Parameters->e_passIndex = Pass;
				Parameters->b_sort = KeySRVs[SrcIndex];
				Parameters->b_globalHist = GlobalHistogramRW;
				Parameters->b_passHist = PassHistogramRW;
				TShaderMapRef<FDeviceRadixUpsweepCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DeviceRadix.Upsweep pass=%u", Pass), Shader, Parameters, FIntVector(ThreadBlocks, 1, 1));
			}

			{
				FDeviceRadixScanCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDeviceRadixScanCS::FParameters>();
				Parameters->e_numKeys = ElementCount;
				Parameters->e_radixShift = RadixShift;
				Parameters->e_threadBlocks = ThreadBlocks;
				Parameters->b_passHist = PassHistogramRW;
				TShaderMapRef<FDeviceRadixScanCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DeviceRadix.Scan pass=%u", Pass), Shader, Parameters, FIntVector(Radix, 1, 1));
			}

			{
				FDeviceRadixDownsweepCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDeviceRadixDownsweepCS::FParameters>();
				Parameters->e_numKeys = ElementCount;
				Parameters->e_radixShift = RadixShift;
				Parameters->e_threadBlocks = ThreadBlocks;
				Parameters->e_passIndex = Pass;
				Parameters->b_sort = KeySRVs[SrcIndex];
				Parameters->b_sortPayload = ValueSRVs[SrcIndex];
				Parameters->b_alt = KeyUAVs[DstIndex];
				Parameters->b_altPayload = bFinalPass && FinalValueUAV ? FinalValueUAV : ValueUAVs[DstIndex];
				Parameters->b_globalHist = GlobalHistogramRW;
				Parameters->b_passHist = PassHistogramRW;
				FDeviceRadixDownsweepCS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FDeviceRadixWriteKeysDim>(
					!bFinalPass || ResolvedOptions.bWriteFinalKeys);
				PermutationVector.Set<FDeviceRadixIdentityPayloadDim>(
					Pass == 0u && ResolvedOptions.bFirstPayloadIsIdentity);
				TShaderMapRef<FDeviceRadixDownsweepCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DeviceRadix.Downsweep pass=%u", Pass), Shader, Parameters, FIntVector(ThreadBlocks, 1, 1));
			}
		}

		if (bOutResultInInputBuffers)
		{
			checkf(!FinalValues,
				TEXT("DeviceRadixSort cannot report a ping-pong result when FinalValues is supplied."));
			*bOutResultInInputBuffers = (ResolvedOptions.PassCount & 1u) == 0u;
		}
	}
}
