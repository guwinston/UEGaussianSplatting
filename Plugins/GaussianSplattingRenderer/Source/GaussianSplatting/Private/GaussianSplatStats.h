#pragma once

#include "CoreMinimal.h"

// Stats group shown via the console command:  stat GaussianSplat
//
// Counters:
//   Splats Before Cull - total number of splats fed into the GPU cull pass each frame.
//   Splats After  Cull - number of splats that survived the cull and feed the sort / draw.
//
// Notes:
//   - "Before Cull" is CPU-known (the merged global splat count) and is updated every frame.
//   - "After Cull"  is produced on the GPU by the per-splat cull / sort-key-generation pass.
//     It is read back asynchronously (FRHIGPUBufferReadback), so the displayed value lags
//     by one or two frames behind the current view.
DECLARE_STATS_GROUP(TEXT("GaussianSplat"), STATGROUP_GaussianSplat, STATCAT_Advanced);

DECLARE_DWORD_COUNTER_STAT(TEXT("Splats Before Cull"), STAT_GaussianSplat_SplatsBeforeCull, STATGROUP_GaussianSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Splats After Cull"),  STAT_GaussianSplat_SplatsAfterCull,  STATGROUP_GaussianSplat);
