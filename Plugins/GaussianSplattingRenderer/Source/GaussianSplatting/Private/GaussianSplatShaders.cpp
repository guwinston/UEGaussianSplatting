#include "GaussianSplatShaders.h"
#include "ShaderParameterUtils.h"
#include "RenderGraphUtils.h"

// Implement the shader types with their permutation domains.

IMPLEMENT_GLOBAL_SHADER(FGaussianObjectCullCS, "/Plugin/GaussianSplattingRenderer/GaussianSplatCullAndSortKeyGen.usf", "CullObjectsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianBuildSortKeysCS, "/Plugin/GaussianSplattingRenderer/GaussianSplatCullAndSortKeyGen.usf", "BuildSortKeysCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianBuildIndirectArgsCS, "/Plugin/GaussianSplattingRenderer/GaussianSplatCullAndSortKeyGen.usf", "BuildIndirectArgsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatVS, "/Plugin/GaussianSplattingRenderer/GaussianSplat.usf", "GaussianSplatVS", SF_Vertex);

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatMS, "/Plugin/GaussianSplattingRenderer/GaussianSplat.usf", "GaussianSplatMS", SF_Mesh);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPS, "/Plugin/GaussianSplattingRenderer/GaussianSplat.usf", "GaussianSplatPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositeVS, "/Plugin/GaussianSplattingRenderer/GaussianSplat.usf", "GaussianSplatCompositeVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositePS, "/Plugin/GaussianSplattingRenderer/GaussianSplat.usf", "GaussianSplatCompositePS", SF_Pixel);
