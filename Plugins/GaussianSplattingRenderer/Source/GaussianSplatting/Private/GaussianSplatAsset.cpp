// GaussianSplatAsset.cpp
//
// Implements UGaussianSplatAsset, the runtime data asset that holds all per-splat
// attributes loaded from a .ply file (positions, covariances, colors, SH coefficients).
//
// Selection-geometry pipeline:
//   BuildConvexHullFromPoints     - fits a minimal convex hull to a point cluster
//   BuildBoxHullFromPoints        - fallback: computes an axis-aligned bounding box hull
//   AppendHullMesh                - merges a hull's vertex/index data into flat arrays
//   BuildMultiConvexApproximation - spatially buckets the point cloud into clusters,
//                                   then builds one convex hull per cluster for physics
//   EstimatePointSpacing          - estimates average nearest-neighbor distance for
//                                   adaptive metaball field tuning
//   FGaussianMetaballField        - Gaussian density field built from the point cloud;
//                                   fed into Marching Cubes to produce a smooth shell mesh
//   BuildSelectionShellFromPoints - runs Marching Cubes over the metaball field to
//                                   produce the per-asset selection shell mesh

#include "GaussianSplatAsset.h"
#include "GaussianSplatPlyLoader.h"
#include "GaussianSplatComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/Paths.h"
#include "Async/ParallelFor.h"
#include "ProfilingDebugging/ScopedTimers.h"
#include <exception>

namespace
{
static constexpr int32 GGaussianSplatCompressedDataVersion = 11;
static constexpr int32 GGaussianSplatMaxSHComponentsPerSplat = 45;

// Scale is stored in UE centimeters, while the original SPZ fixed log-scale
// range is defined for source-meter log-scales:
//   q = round((logScaleMeters + 10) * 16)
//   logScaleMeters = q / 16 - 10
// PLY import already adds ln(100) when converting meters to centimeters, so
// the runtime range is the same SPZ range shifted by ln(100).
static constexpr float GGaussianSplatFixedLogScaleMin = -10.0f + 4.605170185988091368f;
static constexpr float GGaussianSplatFixedLogScaleStep = 1.0f / 16.0f;

static int32 GetSHCoeffCountForDegree(int32 Degree)
{
    if (Degree >= 3) return 15;
    if (Degree >= 2) return 8;
    if (Degree >= 1) return 3;
    return 0;
}

// ============================================================
// GetSHPackedWordsPerSplat
// Returns the number of uint32 words needed to store one splat's direct
// 8-bit higher-order SH coefficients for all RGB channels.
// ============================================================
static int32 GetSHPackedWordsPerSplat(int32 Degree)
{
    const int32 SHCoeffCount = GetSHCoeffCountForDegree(Degree);
    if (SHCoeffCount <= 0)
    {
        return 0;
    }

    return FMath::DivideAndRoundUp(SHCoeffCount * 3, 4);
}

static uint64 EstimateCompressedRenderPayloadBytes(const FGaussianSplatCompressedData& Data)
{
    uint64 TotalBytes = 0;
    TotalBytes += uint64(Data.PackedPositions.Num()) * sizeof(uint16);
    TotalBytes += uint64(Data.PackedColors.Num()) * sizeof(uint32);
    TotalBytes += uint64(Data.PackedRotations.Num()) * sizeof(uint32);
    TotalBytes += uint64(Data.PackedScales.Num()) * sizeof(uint32);
    TotalBytes += uint64(Data.PackedNormals.Num()) * sizeof(uint32);
    TotalBytes += uint64(Data.PackedSHData.Num()) * sizeof(uint32);
    TotalBytes += uint64(Data.ScaleCodebook.Num()) * sizeof(float);
    TotalBytes += uint64(Data.SHCodebook.Num()) * sizeof(float);
    TotalBytes += uint64(Data.ChunkPositionMins.Num()) * sizeof(FVector4f);
    TotalBytes += uint64(Data.ChunkPositionMaxs.Num()) * sizeof(FVector4f);
    return TotalBytes;
}

static uint64 EstimateInriaPlyFloatPayloadBytes(int32 NumSplats, int32 SHCoeffCount)
{
    // Standard binary 3DGS PLY payload: xyz + unused normal + f_dc + f_rest + opacity + scale + rot.
    const int32 FloatsPerSplat = 3 + 3 + 3 + SHCoeffCount * 3 + 1 + 3 + 4;
    return uint64(FMath::Max(0, NumSplats)) * uint64(FloatsPerSplat) * sizeof(float);
}

static void LogCompressionStats(const UObject* Asset, const FGaussianSplatCompressedData& Data)
{
    const uint64 SourceBytes = EstimateInriaPlyFloatPayloadBytes(Data.SplatCount, Data.SHCoefficientsPerChannel);
    const uint64 CompressedBytes = EstimateCompressedRenderPayloadBytes(Data);
    if (SourceBytes == 0 || CompressedBytes == 0)
    {
        return;
    }

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatting: Compressed payload for %s is %.2f MB vs %.2f MB PLY-equivalent float payload (%.2fx smaller)."),
        Asset ? *Asset->GetPathName() : TEXT("<unknown>"),
        double(CompressedBytes) / (1024.0 * 1024.0),
        double(SourceBytes) / (1024.0 * 1024.0),
        double(SourceBytes) / double(CompressedBytes));
}

static uint32 QuantizeUnorm(float Value, uint32 MaxInt)
{
    const float Clamped = FMath::Clamp(Value, 0.0f, 1.0f);
    return (uint32)FMath::RoundToInt(Clamped * (float)MaxInt);
}

static uint32 ExpandBitsForMorton(uint32 Value)
{
    Value &= 0x000003ffu;
    Value = (Value | (Value << 16u)) & 0x030000FFu;
    Value = (Value | (Value << 8u)) & 0x0300F00Fu;
    Value = (Value | (Value << 4u)) & 0x030C30C3u;
    Value = (Value | (Value << 2u)) & 0x09249249u;
    return Value;
}

static uint32 ComputeMortonCode3D(const FVector3f& NormalizedPosition)
{
    const uint32 X = QuantizeUnorm(NormalizedPosition.X, 1023u);
    const uint32 Y = QuantizeUnorm(NormalizedPosition.Y, 1023u);
    const uint32 Z = QuantizeUnorm(NormalizedPosition.Z, 1023u);
    return (ExpandBitsForMorton(X) << 2u) | (ExpandBitsForMorton(Y) << 1u) | ExpandBitsForMorton(Z);
}

struct FMortonSortEntry
{
    uint32 MortonCode = 0;
    int32 SourceIndex = 0;
};

static void BuildMortonSortedIndices(
    const float* Positions,
    int32 NumSplats,
    const FVector3f& BoundsMin,
    const FVector3f& BoundsExtent,
    TArray<int32>& OutSortedIndices)
{
    OutSortedIndices.Reset();
    if (!Positions || NumSplats <= 0)
    {
        return;
    }

    TArray<FMortonSortEntry> Entries;
    TArray<FMortonSortEntry> Scratch;
    Entries.SetNumUninitialized(NumSplats);
    Scratch.SetNumUninitialized(NumSplats);

    // Convert bounds extents to reciprocals once so the per-splat key build is
    // only multiply/add work. Degenerate axes map to the middle of the Morton
    // domain, matching the old comparison-sort path.
    const FVector3f InvBoundsExtent(
        BoundsExtent.X > UE_SMALL_NUMBER ? 1.0f / BoundsExtent.X : 0.0f,
        BoundsExtent.Y > UE_SMALL_NUMBER ? 1.0f / BoundsExtent.Y : 0.0f,
        BoundsExtent.Z > UE_SMALL_NUMBER ? 1.0f / BoundsExtent.Z : 0.0f);

    // Build each splat's Morton key exactly once. The old TArray.Sort comparator
    // recomputed this many times per element, which dominated import time on
    // multi-million-splat assets.
    ParallelFor(NumSplats, [&](int32 SplatIndex)
    {
        const FVector3f NormalizedPosition(
            InvBoundsExtent.X > 0.0f ? (Positions[SplatIndex * 3 + 0] - BoundsMin.X) * InvBoundsExtent.X : 0.5f,
            InvBoundsExtent.Y > 0.0f ? (Positions[SplatIndex * 3 + 1] - BoundsMin.Y) * InvBoundsExtent.Y : 0.5f,
            InvBoundsExtent.Z > 0.0f ? (Positions[SplatIndex * 3 + 2] - BoundsMin.Z) * InvBoundsExtent.Z : 0.5f);

        FMortonSortEntry& Entry = Entries[SplatIndex];
        Entry.MortonCode = ComputeMortonCode3D(NormalizedPosition);
        Entry.SourceIndex = SplatIndex;
    });

    static constexpr int32 RadixBits = 8;
    static constexpr int32 RadixSize = 1 << RadixBits;
    static constexpr int32 RadixMask = RadixSize - 1;

    // Stable LSD radix sort over the uint32 Morton key. Each pass sorts one
    // byte, from least-significant to most-significant, so four linear passes
    // produce the same ordering as sorting by the full 32-bit key.
    for (int32 Shift = 0; Shift < 32; Shift += RadixBits)
    {
        int32 Counts[RadixSize] = {};

        // Count how many entries land in each 8-bit bucket for this pass.
        for (const FMortonSortEntry& Entry : Entries)
        {
            ++Counts[(Entry.MortonCode >> Shift) & RadixMask];
        }

        // Prefix-sum the bucket counts into write offsets. Offsets[bucket] is
        // the next output slot for that bucket in Scratch.
        int32 Offsets[RadixSize];
        int32 RunningOffset = 0;
        for (int32 BucketIndex = 0; BucketIndex < RadixSize; ++BucketIndex)
        {
            Offsets[BucketIndex] = RunningOffset;
            RunningOffset += Counts[BucketIndex];
        }

        // Scatter in input order. Incrementing Offsets after each write keeps
        // the pass stable, which is required for LSD radix sort and preserves
        // original SourceIndex order among identical Morton codes.
        for (const FMortonSortEntry& Entry : Entries)
        {
            const int32 BucketIndex = (Entry.MortonCode >> Shift) & RadixMask;
            Scratch[Offsets[BucketIndex]++] = Entry;
        }

        Swap(Entries, Scratch);
    }

    // Drop the temporary keys; downstream compression only needs the source
    // indices in Morton order.
    OutSortedIndices.SetNumUninitialized(NumSplats);
    ParallelFor(NumSplats, [&](int32 SplatIndex)
    {
        OutSortedIndices[SplatIndex] = Entries[SplatIndex].SourceIndex;
    });
}

static void PackPosition_16_16_16(const FVector3f& NormalizedPosition, uint16* OutPackedPosition)
{
    OutPackedPosition[0] = uint16(QuantizeUnorm(NormalizedPosition.X, 65535u));
    OutPackedPosition[1] = uint16(QuantizeUnorm(NormalizedPosition.Y, 65535u));
    OutPackedPosition[2] = uint16(QuantizeUnorm(NormalizedPosition.Z, 65535u));
}

static uint32 QuantizeFixedLogScale(float LogScale)
{
    // Direct fixed-grid quantization in log space. Values outside the SPZ-style
    // range are clamped to the nearest endpoint instead of being pulled toward
    // an asset-dependent centroid as the old codebook path did.
    return QuantizeUnorm((LogScale - GGaussianSplatFixedLogScaleMin) / (GGaussianSplatFixedLogScaleStep * 255.0f), 255u);
}

static float DecodeFixedLogScale(uint32 QuantizedScale)
{
    // QuantizedScale is one byte from PackedScales. The maximum reconstruction
    // error is half a step in log space: 0.03125, or about 3.2% in linear scale.
    return GGaussianSplatFixedLogScaleMin + float(QuantizedScale & 0xffu) * GGaussianSplatFixedLogScaleStep;
}

static uint32 PackFixedLogScale_8_8_8(const FVector3f& LogScale)
{
    // Pack X/Y/Z scale bytes into the low three bytes of one uint32. The high
    // byte is currently unused and left as zero.
    return QuantizeFixedLogScale(LogScale.X)
        | (QuantizeFixedLogScale(LogScale.Y) << 8u)
        | (QuantizeFixedLogScale(LogScale.Z) << 16u);
}

// ============================================================
// PackSmallestThreeQuaternion
// Quantizes a unit quaternion with the "smallest three" layout:
//   - store the index of the largest-magnitude component in the top 2 bits
//   - drop that largest component
//   - store the remaining three components as 10-bit UNORM values
//
// The omitted component can be reconstructed in the shader because a unit
// quaternion has length 1. This packs rotation into one uint32 while keeping
// enough precision for Gaussian covariance orientation.
// ============================================================
static uint32 PackSmallestThreeQuaternion(const FQuat4f& InQuat)
{
    FQuat4f Quat = InQuat.GetNormalized();

    // Find the component with largest absolute value. Dropping the largest
    // component minimizes reconstruction error because the remaining stored
    // components have the smallest total dynamic range.
    int32 MaxComponent = 0;
    float MaxAbsComponent = FMath::Abs(Quat.X);
    const float Components[4] = { Quat.X, Quat.Y, Quat.Z, Quat.W };
    for (int32 ComponentIndex = 1; ComponentIndex < 4; ++ComponentIndex)
    {
        const float ComponentAbs = FMath::Abs(Components[ComponentIndex]);
        if (ComponentAbs > MaxAbsComponent)
        {
            MaxAbsComponent = ComponentAbs;
            MaxComponent = ComponentIndex;
        }
    }

    // q and -q represent the same rotation. Force the omitted component to be
    // positive so the shader can reconstruct it with a positive square root.
    if (Components[MaxComponent] < 0.0f)
    {
        Quat.X *= -1.0f;
        Quat.Y *= -1.0f;
        Quat.Z *= -1.0f;
        Quat.W *= -1.0f;
    }

    // Copy the three non-omitted components in a deterministic order. The
    // 2-bit MaxComponent field tells the shader which component was omitted.
    float Reduced[3];
    switch (MaxComponent)
    {
    case 0:
        Reduced[0] = Quat.Y;
        Reduced[1] = Quat.Z;
        Reduced[2] = Quat.W;
        break;
    case 1:
        Reduced[0] = Quat.X;
        Reduced[1] = Quat.Z;
        Reduced[2] = Quat.W;
        break;
    case 2:
        Reduced[0] = Quat.X;
        Reduced[1] = Quat.Y;
        Reduced[2] = Quat.W;
        break;
    default:
        Reduced[0] = Quat.X;
        Reduced[1] = Quat.Y;
        Reduced[2] = Quat.Z;
        break;
    }

    // With the largest component omitted, each stored component is guaranteed
    // to lie within [-1/sqrt(2), +1/sqrt(2)]. Map that range to [0, 1] and
    // quantize to 10 bits per component.
    const uint32 A = QuantizeUnorm(Reduced[0] * (0.5f / FMath::Sqrt(2.0f)) + 0.5f, 1023u);
    const uint32 B = QuantizeUnorm(Reduced[1] * (0.5f / FMath::Sqrt(2.0f)) + 0.5f, 1023u);
    const uint32 C = QuantizeUnorm(Reduced[2] * (0.5f / FMath::Sqrt(2.0f)) + 0.5f, 1023u);

    // Bit layout: [31:30] omitted component index, [29:20] A, [19:10] B, [9:0] C.
    return ((uint32)MaxComponent << 30u) | (A << 20u) | (B << 10u) | C;
}

// ============================================================
// EncodeOctahedron
// Encodes a 3D unit normal into a 2D octahedral representation.
// The mapping first projects the normal onto the octahedron surface
// (|x| + |y| + |z| = 1), then unfolds that surface into 2D by keeping
// x/y. Normals on the lower hemisphere (z < 0) are mirrored across the
// corresponding octahedron axes so the full sphere fits into a single
// 2D domain where both coordinates stay in [-1, 1].
// ============================================================
static FVector2f EncodeOctahedron(const FVector3f& InNormal)
{
    // Ensure we start from a valid unit-length direction. Fall back to +Z if
    // the input is degenerate so the later projection stays numerically stable.
    FVector3f Normal = InNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0.0f, 0.0f, 1.0f));

    // Project the unit sphere direction onto the octahedron surface by
    // normalizing with the L1 length. After this step:
    //   |x| + |y| + |z| = 1
    const float InvL1 = 1.0f / (FMath::Abs(Normal.X) + FMath::Abs(Normal.Y) + FMath::Abs(Normal.Z));
    Normal *= InvL1;

    // Unfold the octahedron to 2D by taking the projected x/y coordinates.
    // Upper-hemisphere normals (z >= 0) already lie in the final 2D chart.
    FVector2f Encoded(Normal.X, Normal.Y);
    if (Normal.Z < 0.0f)
    {
        // Mirror the lower hemisphere across the octahedron axes so both
        // hemispheres share the same 2D domain. This folds the full sphere
        // into a single square-like region with x/y in [-1, 1].
        Encoded = FVector2f(
            (1.0f - FMath::Abs(Encoded.Y)) * FMath::Sign(Encoded.X),
            (1.0f - FMath::Abs(Encoded.X)) * FMath::Sign(Encoded.Y));
    }

    return Encoded;
}

static uint32 PackNormal(const FVector3f& Normal)
{
    const FVector2f Oct = EncodeOctahedron(Normal);
    const uint32 OctX = QuantizeUnorm(Oct.X * 0.5f + 0.5f, 255u);
    const uint32 OctY = QuantizeUnorm(Oct.Y * 0.5f + 0.5f, 255u);
    return OctX | (OctY << 8u);
}

static FVector3f DecodePackedPosition(const FGaussianSplatCompressedData& Data, int32 SplatIndex)
{
    const uint16* PackedPosition = Data.PackedPositions.GetData() + SplatIndex * 3;
    const int32 ChunkIndex = SplatIndex / (int32)FGaussianSplatCompressedData::ChunkSize;

    const FVector3f PositionMin = FVector3f(Data.ChunkPositionMins[ChunkIndex]);
    const FVector3f PositionMax = FVector3f(Data.ChunkPositionMaxs[ChunkIndex]);

    const FVector3f Normalized(
        float(PackedPosition[0]) / 65535.0f,
        float(PackedPosition[1]) / 65535.0f,
        float(PackedPosition[2]) / 65535.0f);

    return FMath::Lerp(PositionMin, PositionMax, Normalized);
}

struct FChunkPositionQuantizationStat
{
    int32 ChunkIndex = INDEX_NONE;
    FVector3f ExtentCm = FVector3f::ZeroVector;
    FVector3f MaxAbsErrorCm = FVector3f::ZeroVector;
    float WorstAxisErrorCm = 0.0f;
};

static void LogPositionQuantizationStats(const UObject* Asset, const FGaussianSplatCompressedData& Data)
{
    const int32 ChunkCount = Data.GetChunkCount();
    if (ChunkCount <= 0)
    {
        return;
    }

    FVector3f SumExtentCm = FVector3f::ZeroVector;
    FVector3f MaxExtentCm = FVector3f::ZeroVector;
    FVector3f SumErrorCm = FVector3f::ZeroVector;
    FVector3f MaxErrorCm = FVector3f::ZeroVector;
    int32 ChunksAbove01Cm = 0;
    int32 ChunksAbove025Cm = 0;
    int32 ChunksAbove05Cm = 0;
    int32 ChunksAbove1Cm = 0;

    TArray<FChunkPositionQuantizationStat> ChunkStats;
    ChunkStats.Reserve(ChunkCount);

    for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
    {
        const FVector3f PositionMin(Data.ChunkPositionMins[ChunkIndex]);
        const FVector3f PositionMax(Data.ChunkPositionMaxs[ChunkIndex]);
        const FVector3f ExtentCm = PositionMax - PositionMin;
        const FVector3f ErrorCm(
            ExtentCm.X / 131070.0f,
            ExtentCm.Y / 131070.0f,
            ExtentCm.Z / 131070.0f);
        const float WorstAxisErrorCm = FMath::Max3(ErrorCm.X, ErrorCm.Y, ErrorCm.Z);

        SumExtentCm += ExtentCm;
        MaxExtentCm.X = FMath::Max(MaxExtentCm.X, ExtentCm.X);
        MaxExtentCm.Y = FMath::Max(MaxExtentCm.Y, ExtentCm.Y);
        MaxExtentCm.Z = FMath::Max(MaxExtentCm.Z, ExtentCm.Z);

        SumErrorCm += ErrorCm;
        MaxErrorCm.X = FMath::Max(MaxErrorCm.X, ErrorCm.X);
        MaxErrorCm.Y = FMath::Max(MaxErrorCm.Y, ErrorCm.Y);
        MaxErrorCm.Z = FMath::Max(MaxErrorCm.Z, ErrorCm.Z);

        ChunksAbove01Cm += WorstAxisErrorCm >= 0.1f ? 1 : 0;
        ChunksAbove025Cm += WorstAxisErrorCm >= 0.25f ? 1 : 0;
        ChunksAbove05Cm += WorstAxisErrorCm >= 0.5f ? 1 : 0;
        ChunksAbove1Cm += WorstAxisErrorCm >= 1.0f ? 1 : 0;

        FChunkPositionQuantizationStat& Stat = ChunkStats.Emplace_GetRef();
        Stat.ChunkIndex = ChunkIndex;
        Stat.ExtentCm = ExtentCm;
        Stat.MaxAbsErrorCm = ErrorCm;
        Stat.WorstAxisErrorCm = WorstAxisErrorCm;
    }

    const float InvChunkCount = 1.0f / float(ChunkCount);
    const FVector3f AvgExtentCm = SumExtentCm * InvChunkCount;
    const FVector3f AvgErrorCm = SumErrorCm * InvChunkCount;

    UE_LOG(
        LogTemp,
        Log,
        TEXT("GaussianSplatting: Position quantization stats for %s: %d chunks (chunk size %u), avg extent=(%.2f, %.2f, %.2f) cm, max extent=(%.2f, %.2f, %.2f) cm, avg max-abs-error=(%.4f, %.4f, %.4f) cm, max max-abs-error=(%.4f, %.4f, %.4f) cm, worst-axis thresholds: >=0.1cm=%d, >=0.25cm=%d, >=0.5cm=%d, >=1.0cm=%d"),
        Asset ? *Asset->GetName() : TEXT("<null>"),
        ChunkCount,
        FGaussianSplatCompressedData::ChunkSize,
        AvgExtentCm.X, AvgExtentCm.Y, AvgExtentCm.Z,
        MaxExtentCm.X, MaxExtentCm.Y, MaxExtentCm.Z,
        AvgErrorCm.X, AvgErrorCm.Y, AvgErrorCm.Z,
        MaxErrorCm.X, MaxErrorCm.Y, MaxErrorCm.Z,
        ChunksAbove01Cm, ChunksAbove025Cm, ChunksAbove05Cm, ChunksAbove1Cm);

    ChunkStats.Sort([](const FChunkPositionQuantizationStat& A, const FChunkPositionQuantizationStat& B)
    {
        if (A.WorstAxisErrorCm == B.WorstAxisErrorCm)
        {
            return A.ChunkIndex < B.ChunkIndex;
        }
        return A.WorstAxisErrorCm > B.WorstAxisErrorCm;
    });

    const int32 WorstChunkLogCount = FMath::Min(5, ChunkStats.Num());
    for (int32 Index = 0; Index < WorstChunkLogCount; ++Index)
    {
        const FChunkPositionQuantizationStat& Stat = ChunkStats[Index];
        UE_LOG(
            LogTemp,
            Log,
            TEXT("GaussianSplatting:   Worst chunk #%d: extent=(%.2f, %.2f, %.2f) cm, theoretical max abs error=(%.4f, %.4f, %.4f) cm, worst-axis=%.4f cm"),
            Stat.ChunkIndex,
            Stat.ExtentCm.X, Stat.ExtentCm.Y, Stat.ExtentCm.Z,
            Stat.MaxAbsErrorCm.X, Stat.MaxAbsErrorCm.Y, Stat.MaxAbsErrorCm.Z,
            Stat.WorstAxisErrorCm);
    }
}

static void LogSHQuantizationStats(
    const UObject* Asset,
    const FGaussianSplatCompressedData& Data,
    const float* SourceSHData,
    const TArray<int32>& SortedIndices)
{
    if (!SourceSHData
        || Data.SHCoefficientsPerChannel <= 0
        || Data.SHPackedWordsPerSplat <= 0
        || Data.PackedSHData.IsEmpty()
        || Data.SHCodebook.Num() != FGaussianSplatCompressedData::SHCodebookSize
        || SortedIndices.Num() != Data.SplatCount)
    {
        return;
    }

    const int32 ComponentsPerSplat = Data.SHCoefficientsPerChannel * 3;
    double SumAbsError = 0.0;
    double SumSquaredError = 0.0;
    float MaxAbsError = 0.0f;
    int32 MaxErrorSourceSplatIndex = INDEX_NONE;
    int32 MaxErrorComponentIndex = INDEX_NONE;

    for (int32 SplatIndex = 0; SplatIndex < Data.SplatCount; ++SplatIndex)
    {
        const int32 SourceIndex = SortedIndices[SplatIndex];
        const int32 SourceBase = SourceIndex * ComponentsPerSplat;
        const uint32* PackedWords = Data.PackedSHData.GetData() + SplatIndex * Data.SHPackedWordsPerSplat;
        for (int32 ComponentIndex = 0; ComponentIndex < ComponentsPerSplat; ++ComponentIndex)
        {
            const uint32 PackedWord = PackedWords[ComponentIndex / 4];
            const uint32 CodebookIndex = (PackedWord >> ((ComponentIndex & 3) * 8)) & 0xffu;
            const float DecodedValue = Data.SHCodebook[CodebookIndex];
            const float AbsError = FMath::Abs(DecodedValue - SourceSHData[SourceBase + ComponentIndex]);
            SumAbsError += double(AbsError);
            SumSquaredError += double(AbsError) * double(AbsError);
            if (AbsError > MaxAbsError)
            {
                MaxAbsError = AbsError;
                MaxErrorSourceSplatIndex = SourceIndex;
                MaxErrorComponentIndex = ComponentIndex;
            }
        }
    }

    const int64 TotalComponentCount = int64(Data.SplatCount) * int64(ComponentsPerSplat);
    const double InvTotalComponentCount = TotalComponentCount > 0 ? 1.0 / double(TotalComponentCount) : 0.0;
    const float QuantStep = Data.SHCodebook.Num() > 1
        ? FMath::Abs(Data.SHCodebook[1] - Data.SHCodebook[0])
        : 0.0f;

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatting: SH quantization stats for %s: max_abs=%.8f at source splat %d component %d, mean_abs=%.8f, rmse=%.8f, quant_step=%.8f."),
        Asset ? *Asset->GetName() : TEXT("<null>"),
        double(MaxAbsError),
        MaxErrorSourceSplatIndex,
        MaxErrorComponentIndex,
        SumAbsError * InvTotalComponentCount,
        FMath::Sqrt(SumSquaredError * InvTotalComponentCount),
        double(QuantStep));
}

static void LogScaleQuantizationStats(
    const UObject* Asset,
    const FGaussianSplatCompressedData& Data,
    const float* SourceLogScales,
    const TArray<int32>& SortedIndices)
{
    if (!SourceLogScales
        || Data.PackedScales.Num() != Data.SplatCount
        || Data.ScaleCodebook.Num() != FGaussianSplatCompressedData::ScaleDummyValueCount
        || SortedIndices.Num() != Data.SplatCount)
    {
        return;
    }

    double SumAbsLogError = 0.0;
    double SumSquaredLogError = 0.0;
    float MaxAbsLogError = 0.0f;
    int32 MaxErrorSourceSplatIndex = INDEX_NONE;
    int32 MaxErrorAxis = INDEX_NONE;
    float SourceLogAtMaxError = 0.0f;
    float DecodedLogAtMaxError = 0.0f;

    for (int32 SplatIndex = 0; SplatIndex < Data.SplatCount; ++SplatIndex)
    {
        const int32 SourceIndex = SortedIndices[SplatIndex];
        const uint32 PackedScale = Data.PackedScales[SplatIndex];
        const uint32 ScaleIndices[3] =
        {
            PackedScale & 0xffu,
            (PackedScale >> 8u) & 0xffu,
            (PackedScale >> 16u) & 0xffu
        };

        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            const float SourceLogScale = SourceLogScales[SourceIndex * 3 + Axis];
            const float DecodedLogScale = DecodeFixedLogScale(ScaleIndices[Axis]);
            const float AbsLogError = FMath::Abs(DecodedLogScale - SourceLogScale);
            SumAbsLogError += double(AbsLogError);
            SumSquaredLogError += double(AbsLogError) * double(AbsLogError);

            if (AbsLogError > MaxAbsLogError)
            {
                MaxAbsLogError = AbsLogError;
                MaxErrorSourceSplatIndex = SourceIndex;
                MaxErrorAxis = Axis;
                SourceLogAtMaxError = SourceLogScale;
                DecodedLogAtMaxError = DecodedLogScale;
            }
        }
    }

    const int64 TotalScaleCount = int64(Data.SplatCount) * 3;
    const double InvTotalScaleCount = TotalScaleCount > 0 ? 1.0 / double(TotalScaleCount) : 0.0;
    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatting: Scale quantization stats for %s: max_abs_log=%.8f at source splat %d axis %d (source %.8f -> decoded %.8f, ratio %.6f), mean_abs_log=%.8f, rmse_log=%.8f."),
        Asset ? *Asset->GetName() : TEXT("<null>"),
        double(MaxAbsLogError),
        MaxErrorSourceSplatIndex,
        MaxErrorAxis,
        double(SourceLogAtMaxError),
        double(DecodedLogAtMaxError),
        double(FMath::Exp(DecodedLogAtMaxError - SourceLogAtMaxError)),
        SumAbsLogError * InvTotalScaleCount,
        FMath::Sqrt(SumSquaredLogError * InvTotalScaleCount));
}

static void LogCompressedAttributeStats(
    const UObject* Asset,
    const FGaussianSplatCompressedData& Data,
    const float* SourceLogScales,
    const float* SourceSHData,
    const TArray<int32>& SortedIndices)
{
    LogPositionQuantizationStats(Asset, Data);
    LogScaleQuantizationStats(Asset, Data, SourceLogScales, SortedIndices);
    LogSHQuantizationStats(Asset, Data, SourceSHData, SortedIndices);
    LogCompressionStats(Asset, Data);
}

// ============================================================
// BuildDirectSHStreamFromSource
// Builds the direct higher-order SH stream used by the runtime renderer.
// The importer quantizes every source SH scalar to an 8-bit index in a
// per-asset linear codebook spanning [SHMin, SHMax], then packs four indices
// per uint32. This path stores one SH entry per splat; it does not use k-means,
// palette labels, or SPZ's fixed [-1, 1] signed-byte SH formula.
// ============================================================
static bool BuildDirectSHStreamFromSource(
    const float* SHData,
    const TArray<int32>& SortedIndices,
    int32 NumSplats,
    int32 ComponentsPerSplat,
    float SHMin,
    float SHMax,
    TArray<uint32>& OutPackedSH,
    TArray<float>& OutCodebook)
{
    OutPackedSH.Reset();
    OutCodebook.Reset();

    if (!SHData || NumSplats <= 0 || ComponentsPerSplat <= 0 || SortedIndices.Num() != NumSplats)
    {
        return false;
    }

    check(ComponentsPerSplat <= GGaussianSplatMaxSHComponentsPerSplat);

    if (FMath::Abs(SHMax - SHMin) < UE_SMALL_NUMBER)
    {
        SHMax = SHMin + 1.0f;
    }

    const int32 PackedWordsPerSplat = FMath::DivideAndRoundUp(ComponentsPerSplat, 4);
    OutPackedSH.SetNumZeroed(NumSplats * PackedWordsPerSplat);
    OutCodebook.SetNumUninitialized(FGaussianSplatCompressedData::SHCodebookSize);

    const float InvCodebookMax = 1.0f / float(FGaussianSplatCompressedData::SHCodebookSize - 1);
    for (int32 CodebookIndex = 0; CodebookIndex < FGaussianSplatCompressedData::SHCodebookSize; ++CodebookIndex)
    {
        OutCodebook[CodebookIndex] = FMath::Lerp(SHMin, SHMax, float(CodebookIndex) * InvCodebookMax);
    }

    const float QuantScale = 255.0f / (SHMax - SHMin);
    ParallelFor(NumSplats, [&](int32 SplatIndex)
    {
        const int32 SourceIndex = SortedIndices[SplatIndex];
        const int32 SourceBase = SourceIndex * ComponentsPerSplat;
        uint32* PackedWords = OutPackedSH.GetData() + SplatIndex * PackedWordsPerSplat;
        for (int32 ComponentIndex = 0; ComponentIndex < ComponentsPerSplat; ++ComponentIndex)
        {
            const uint32 CodebookIndex = uint32(FMath::Clamp(
                FMath::RoundToInt((SHData[SourceBase + ComponentIndex] - SHMin) * QuantScale),
                0,
                255));
            PackedWords[ComponentIndex / 4] |= CodebookIndex << ((ComponentIndex & 3) * 8);
        }
    });

    return true;
}
}

#include "PhysicsEngine/BodySetup.h"
#if WITH_EDITOR
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#endif

#include "CompGeom/ConvexHull3.h"
#include "Containers/Queue.h"
#include "Generators/MarchingCubes.h"
#if WITH_EDITOR
#include "Misc/ScopedSlowTask.h"
#endif

namespace
{
using namespace UE::Geometry;

// ============================================================
// BuildConvexHullFromPoints
// Computes the minimum convex hull of the supplied point set using
// UE's TConvexHull3 algorithm.  Only hull-boundary vertices are
// retained; their indices are remapped to a compact range so the
// output arrays carry no unreferenced data.
// Returns false if the point set is degenerate (fewer than 4 points
// or co-planar / numerically singular).
// ============================================================
static bool BuildConvexHullFromPoints(const TArray<FVector3f>& Points, FGaussianSplatConvexHull& OutHull)
{
    OutHull.Vertices.Reset();
    OutHull.Indices.Reset();

    if (Points.Num() < 4)
    {
        return false;
    }

    TConvexHull3<float> Hull;
    if (!Hull.Solve<FVector3f>(Points))
    {
        return false;
    }

    TArray<FIndex3i> HullTris = Hull.MoveTriangles();
    if (HullTris.IsEmpty())
    {
        return false;
    }

    // Remap original point indices to a compact set containing only hull boundary vertices.
    TMap<int32, int32> OldToNew;
    for (const FIndex3i& Tri : HullTris)
    {
        if (!OldToNew.Contains(Tri.A)) { const int32 NewIndex = OldToNew.Num(); OldToNew.Add(Tri.A, NewIndex); }
        if (!OldToNew.Contains(Tri.B)) { const int32 NewIndex = OldToNew.Num(); OldToNew.Add(Tri.B, NewIndex); }
        if (!OldToNew.Contains(Tri.C)) { const int32 NewIndex = OldToNew.Num(); OldToNew.Add(Tri.C, NewIndex); }
    }

    OutHull.Vertices.SetNumUninitialized(OldToNew.Num());
    for (const TPair<int32, int32>& Pair : OldToNew)
    {
        OutHull.Vertices[Pair.Value] = Points[Pair.Key];
    }

    // Flatten triangle list into a flat index buffer.
    OutHull.Indices.SetNumUninitialized(HullTris.Num() * 3);
    for (int32 TriIndex = 0; TriIndex < HullTris.Num(); ++TriIndex)
    {
        OutHull.Indices[TriIndex * 3 + 0] = (uint32)OldToNew[HullTris[TriIndex].A];
        OutHull.Indices[TriIndex * 3 + 1] = (uint32)OldToNew[HullTris[TriIndex].B];
        OutHull.Indices[TriIndex * 3 + 2] = (uint32)OldToNew[HullTris[TriIndex].C];
    }

    return true;
}

// ============================================================
// BuildBoxHullFromPoints
// Fallback hull builder used when the point cluster is too small
// or geometrically degenerate for a proper convex hull.
// Computes the axis-aligned bounding box (AABB) and expresses it
// as 8 vertices and 12 triangles (2 per face).
// Returns false if any axis extent is effectively zero.
// ============================================================
static bool BuildBoxHullFromPoints(const TArray<FVector3f>& Points, FGaussianSplatConvexHull& OutHull)
{
    if (Points.Num() < 4)
    {
        return false;
    }

    // Find per-axis extremes to build the AABB.
    FVector3f MinP(FLT_MAX, FLT_MAX, FLT_MAX);
    FVector3f MaxP(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const FVector3f& Point : Points)
    {
        MinP.X = FMath::Min(MinP.X, Point.X);
        MinP.Y = FMath::Min(MinP.Y, Point.Y);
        MinP.Z = FMath::Min(MinP.Z, Point.Z);
        MaxP.X = FMath::Max(MaxP.X, Point.X);
        MaxP.Y = FMath::Max(MaxP.Y, Point.Y);
        MaxP.Z = FMath::Max(MaxP.Z, Point.Z);
    }

    const FVector3f Extents = MaxP - MinP;
    if (Extents.X <= KINDA_SMALL_NUMBER || Extents.Y <= KINDA_SMALL_NUMBER || Extents.Z <= KINDA_SMALL_NUMBER)
    {
        return false;
    }

    // 8 corners of the AABB, wound so outward normals point away from the box.
    OutHull.Vertices = {
        FVector3f(MinP.X, MinP.Y, MinP.Z), FVector3f(MaxP.X, MinP.Y, MinP.Z),
        FVector3f(MaxP.X, MaxP.Y, MinP.Z), FVector3f(MinP.X, MaxP.Y, MinP.Z),
        FVector3f(MinP.X, MinP.Y, MaxP.Z), FVector3f(MaxP.X, MinP.Y, MaxP.Z),
        FVector3f(MaxP.X, MaxP.Y, MaxP.Z), FVector3f(MinP.X, MaxP.Y, MaxP.Z)
    };
    // 6 faces × 2 triangles each.
    OutHull.Indices = {
        0, 2, 1, 0, 3, 2,   // -Z face
        4, 5, 6, 4, 6, 7,   // +Z face
        0, 1, 5, 0, 5, 4,   // -Y face
        1, 2, 6, 1, 6, 5,   // +X face
        2, 3, 7, 2, 7, 6,   // +Y face
        3, 0, 4, 3, 4, 7    // -X face
    };
    return true;
}

// ============================================================
// AppendHullMesh
// Merges the vertex and index data from a single FGaussianSplatConvexHull
// into flat output arrays, rebasing index values by the current vertex count
// so multiple hulls can be concatenated without overlap.
// ============================================================
static void AppendHullMesh(const FGaussianSplatConvexHull& Hull, TArray<FVector3f>& OutVertices, TArray<uint32>& OutIndices)
{
    const uint32 BaseVertex = (uint32)OutVertices.Num();
    OutVertices.Append(Hull.Vertices);
    OutIndices.Reserve(OutIndices.Num() + Hull.Indices.Num());
    for (uint32 IndexValue : Hull.Indices)
    {
        OutIndices.Add(BaseVertex + IndexValue);
    }
}

// ============================================================
// BuildMultiConvexApproximation
// Decomposes a large point cloud into spatially coherent clusters
// and builds one convex hull per cluster, up to MaxConvexHullCount.
//
// Algorithm:
//   1. Assign every point to a uniform 3-D grid cell based on CellSize,
//      which is derived from the longest bounding-box axis.
//   2. Flood-fill (BFS) over neighbouring cells to group connected cells
//      into clusters.
//   3. Sort clusters by size (largest first); skip tiny ones below
//      SmallClusterThreshold if a proper convex hull cannot be built.
//   4. For each cluster attempt BuildConvexHullFromPoints; fall back to
//      BuildBoxHullFromPoints for small / degenerate clusters.
//   5. If no cluster hull succeeds, fall back to a single hull over all
//      points.
//
// OutFallbackVertices / OutFallbackIndices receive the concatenated mesh
// of all successfully built hulls (used by the scene proxy).
// ============================================================
static void BuildMultiConvexApproximation(
    const TArray<FVector3f>& Positions3f,
    const FBox& Bounds,
    int32 MaxConvexHullCount,
    TArray<FGaussianSplatConvexHull>& OutConvexHulls,
    TArray<FVector3f>& OutFallbackVertices,
    TArray<uint32>& OutFallbackIndices)
{
    OutConvexHulls.Reset();
    OutFallbackVertices.Reset();
    OutFallbackIndices.Reset();

    if (Positions3f.IsEmpty())
    {
        return;
    }

    const FVector Extent = Bounds.GetExtent();
    const float MaxExtent = FMath::Max3(Extent.X, Extent.Y, Extent.Z);
    // Cell size is at least 200 UU, or 1/8th of the longest axis — whichever is larger.
    const float CellSize = FMath::Max(200.0f, MaxExtent / 8.0f);
    const int32 SmallClusterThreshold = 24;          // Minimum points for AABB fallback
    const int32 MaxHullVerticesPerCluster = 512;      // Cap per-cluster samples for performance
    MaxConvexHullCount = FMath::Clamp(MaxConvexHullCount, 1, 128);

    // ---- Step 1: Spatial bucketing ----
    TMap<FIntVector, TArray<int32>> Buckets;
    Buckets.Reserve(Positions3f.Num() / 16 + 1);
    for (int32 Index = 0; Index < Positions3f.Num(); ++Index)
    {
        const FVector Position(Positions3f[Index]);
        const FVector Relative = (Position - Bounds.Min) / CellSize;
        const FIntVector Cell(
            FMath::FloorToInt(Relative.X),
            FMath::FloorToInt(Relative.Y),
            FMath::FloorToInt(Relative.Z));
        Buckets.FindOrAdd(Cell).Add(Index);
    }

    // ---- Step 2: BFS flood-fill to form clusters ----
    TSet<FIntVector> Visited;
    TArray<TArray<int32>> Clusters;
    for (const TPair<FIntVector, TArray<int32>>& BucketPair : Buckets)
    {
        if (Visited.Contains(BucketPair.Key))
        {
            continue;
        }

        TArray<int32> ClusterIndices;
        TQueue<FIntVector> Pending;
        Pending.Enqueue(BucketPair.Key);
        Visited.Add(BucketPair.Key);

        FIntVector Cell;
        while (Pending.Dequeue(Cell))
        {
            if (const TArray<int32>* BucketPoints = Buckets.Find(Cell))
            {
                ClusterIndices.Append(*BucketPoints);
            }

            // Expand into all 26 face/edge/corner neighbours.
            for (int32 DX = -1; DX <= 1; ++DX)
            {
                for (int32 DY = -1; DY <= 1; ++DY)
                {
                    for (int32 DZ = -1; DZ <= 1; ++DZ)
                    {
                        const FIntVector Neighbor = Cell + FIntVector(DX, DY, DZ);
                        if (Buckets.Contains(Neighbor) && !Visited.Contains(Neighbor))
                        {
                            Visited.Add(Neighbor);
                            Pending.Enqueue(Neighbor);
                        }
                    }
                }
            }
        }

        if (!ClusterIndices.IsEmpty())
        {
            Clusters.Add(MoveTemp(ClusterIndices));
        }
    }

    // ---- Step 3: Sort clusters largest-first ----
    Clusters.Sort([](const TArray<int32>& A, const TArray<int32>& B)
    {
        return A.Num() > B.Num();
    });

    // ---- Step 4: Build one convex hull per cluster ----
    for (const TArray<int32>& Cluster : Clusters)
    {
        if (OutConvexHulls.Num() >= MaxConvexHullCount)
        {
            break;
        }

        // Uniformly subsample oversized clusters to stay within the per-cluster vertex budget.
        TArray<FVector3f> ClusterPoints;
        if (Cluster.Num() <= MaxHullVerticesPerCluster)
        {
            ClusterPoints.Reserve(Cluster.Num());
            for (int32 PointIndex : Cluster)
            {
                ClusterPoints.Add(Positions3f[PointIndex]);
            }
        }
        else
        {
            ClusterPoints.Reserve(MaxHullVerticesPerCluster);
            const float Step = float(Cluster.Num()) / float(MaxHullVerticesPerCluster);
            for (int32 SampleIndex = 0; SampleIndex < MaxHullVerticesPerCluster; ++SampleIndex)
            {
                const int32 PointIndex = Cluster[FMath::Min(FMath::FloorToInt(SampleIndex * Step), Cluster.Num() - 1)];
                ClusterPoints.Add(Positions3f[PointIndex]);
            }
        }

        FGaussianSplatConvexHull Hull;
        bool bBuiltHull = BuildConvexHullFromPoints(ClusterPoints, Hull);
        // Fall back to AABB hull only if the cluster has enough points to be meaningful.
        if (!bBuiltHull && Cluster.Num() >= SmallClusterThreshold)
        {
            bBuiltHull = BuildBoxHullFromPoints(ClusterPoints, Hull);
        }

        if (bBuiltHull)
        {
            AppendHullMesh(Hull, OutFallbackVertices, OutFallbackIndices);
            OutConvexHulls.Add(MoveTemp(Hull));
        }
    }

    // ---- Step 5: Last-resort global fallback ----
    if (OutConvexHulls.IsEmpty())
    {
        FGaussianSplatConvexHull Hull;
        if (BuildConvexHullFromPoints(Positions3f, Hull))
        {
            AppendHullMesh(Hull, OutFallbackVertices, OutFallbackIndices);
            OutConvexHulls.Add(MoveTemp(Hull));
        }
    }
}

// ============================================================
// EstimatePointSpacing
// Estimates the average nearest-neighbour distance across the point
// cloud using a spatial hash grid for fast candidate lookup.
//
// The result is used by BuildSelectionShellFromPoints to tune the
// Gaussian influence radius so the metaball field adapts to the
// actual density of the input point cloud rather than using a fixed
// world-space constant.
//
// For very sparse inputs the function returns a fraction of the
// bounding-box diagonal as a safe default.
// ============================================================
static double EstimatePointSpacing(const TArray<FVector3f>& Positions3f, const FBox& Bounds)
{
    if (Positions3f.Num() < 2)
    {
        return FMath::Max(20.0, Bounds.GetExtent().GetMax() * 0.1);
    }

    // Rough grid cell size: one step along the diagonal divided by the cube-root of N.
    const double RoughSpacing = FMath::Max(10.0, Bounds.GetSize().GetMax() / FMath::Max(1.0, FMath::Pow((double)Positions3f.Num(), 1.0 / 3.0)));
    const double SampleCellSize = RoughSpacing * 2.0;

    TMap<FIntVector, TArray<int32>> Buckets;
    Buckets.Reserve(Positions3f.Num() / 8 + 1);
    for (int32 Index = 0; Index < Positions3f.Num(); ++Index)
    {
        const FVector Position(Positions3f[Index]);
        const FVector Relative = (Position - Bounds.Min) / SampleCellSize;
        const FIntVector Cell(
            FMath::FloorToInt(Relative.X),
            FMath::FloorToInt(Relative.Y),
            FMath::FloorToInt(Relative.Z));
        Buckets.FindOrAdd(Cell).Add(Index);
    }

    // Sample at most 256 evenly-spaced points and find each one's nearest neighbour.
    const int32 Step = FMath::Max(1, Positions3f.Num() / 256);
    double SumNearest = 0.0;
    int32 NumSamples = 0;

    for (int32 SampleIndex = 0; SampleIndex < Positions3f.Num(); SampleIndex += Step)
    {
        const FVector Position(Positions3f[SampleIndex]);
        const FVector Relative = (Position - Bounds.Min) / SampleCellSize;
        const FIntVector Cell(
            FMath::FloorToInt(Relative.X),
            FMath::FloorToInt(Relative.Y),
            FMath::FloorToInt(Relative.Z));

        double BestDistSq = TNumericLimits<double>::Max();
        // Search the 3×3×3 neighbourhood to find the nearest candidate.
        for (int32 DX = -1; DX <= 1; ++DX)
        {
            for (int32 DY = -1; DY <= 1; ++DY)
            {
                for (int32 DZ = -1; DZ <= 1; ++DZ)
                {
                    const FIntVector Neighbor = Cell + FIntVector(DX, DY, DZ);
                    if (const TArray<int32>* BucketPoints = Buckets.Find(Neighbor))
                    {
                        for (int32 CandidateIndex : *BucketPoints)
                        {
                            if (CandidateIndex == SampleIndex)
                            {
                                continue;
                            }
                            const double DistSq = FVector::DistSquared(Position, FVector(Positions3f[CandidateIndex]));
                            BestDistSq = FMath::Min(BestDistSq, DistSq);
                        }
                    }
                }
            }
        }

        if (BestDistSq < TNumericLimits<double>::Max())
        {
            SumNearest += FMath::Sqrt(BestDistSq);
            ++NumSamples;
        }
    }

    return NumSamples > 0 ? SumNearest / double(NumSamples) : RoughSpacing;
}

// ============================================================
// FGaussianMetaballField
// A continuous 3-D Gaussian density field built from a discrete
// point cloud.  Each input point contributes a radially symmetric
// Gaussian "blob" to the field; the total density at any world
// position is the sum of all blob contributions within
// InfluenceRadius.
//
// A spatial hash (Buckets) limits Evaluate() to searching only the
// cells within SearchRadiusInCells of the query point, making the
// per-sample cost independent of the total point count.
//
// Usage:
//   1. Call Initialize() once per asset to build the bucket grid.
//   2. Pass the Evaluate() functor to FMarchingCubes::Implicit so
//      the iso-surface extraction sees the density field.
// ============================================================
struct FGaussianMetaballField
{
    FVector3d Origin = FVector3d::Zero();
    double CellSize = 50.0;
    double Sigma = 20.0;
    double InfluenceRadius = 50.0;
    int32 SearchRadiusInCells = 1;
    const TArray<FVector3f>* Positions = nullptr;
    TMap<FIntVector, TArray<int32>> Buckets;

    // Build the spatial hash from the supplied point array.
    // CellSize should equal InfluenceRadius for best bucket balance.
    void Initialize(const TArray<FVector3f>& InPositions, const FBox& Bounds, double InCellSize, double InSigma, double InInfluenceRadius)
    {
        Positions = &InPositions;
        Origin = FVector3d(Bounds.Min);
        CellSize = InCellSize;
        Sigma = InSigma;
        InfluenceRadius = InInfluenceRadius;
        // Number of cells to search in each axis direction.
        SearchRadiusInCells = FMath::Max(1, FMath::CeilToInt(InfluenceRadius / CellSize));
        Buckets.Reset();
        Buckets.Reserve(InPositions.Num() / 8 + 1);

        for (int32 Index = 0; Index < InPositions.Num(); ++Index)
        {
            const FVector3d Relative = (FVector3d(InPositions[Index]) - Origin) / CellSize;
            const FIntVector Cell(
                FMath::FloorToInt(Relative.X),
                FMath::FloorToInt(Relative.Y),
                FMath::FloorToInt(Relative.Z));
            Buckets.FindOrAdd(Cell).Add(Index);
        }
    }

    // Returns the summed Gaussian density at Position.
    // Only points within InfluenceRadius contribute (hard cutoff for performance).
    // density_i = exp(-dist_i^2 / (2*sigma^2))
    double Evaluate(const FVector3d& Position) const
    {
        if (!Positions)
        {
            return 0.0;
        }

        const FVector3d Relative = (Position - Origin) / CellSize;
        const FIntVector CenterCell(
            FMath::FloorToInt(Relative.X),
            FMath::FloorToInt(Relative.Y),
            FMath::FloorToInt(Relative.Z));

        const double InvTwoSigmaSq = 1.0 / (2.0 * Sigma * Sigma);
        const double MaxDistSq = InfluenceRadius * InfluenceRadius;
        double Density = 0.0;

        for (int32 DX = -SearchRadiusInCells; DX <= SearchRadiusInCells; ++DX)
        {
            for (int32 DY = -SearchRadiusInCells; DY <= SearchRadiusInCells; ++DY)
            {
                for (int32 DZ = -SearchRadiusInCells; DZ <= SearchRadiusInCells; ++DZ)
                {
                    const FIntVector Cell = CenterCell + FIntVector(DX, DY, DZ);
                    if (const TArray<int32>* BucketPoints = Buckets.Find(Cell))
                    {
                        for (int32 PointIndex : *BucketPoints)
                        {
                            const FVector3d Delta = FVector3d((*Positions)[PointIndex]) - Position;
                            const double DistSq = Delta.SquaredLength();
                            if (DistSq <= MaxDistSq)
                            {
                                Density += FMath::Exp(-DistSq * InvTwoSigmaSq);
                            }
                        }
                    }
                }
            }
        }

        return Density;
    }
};

// ============================================================
// BuildSelectionShellFromPoints
// Generates a smooth triangulated surface ("selection shell") that
// tightly wraps the point cloud.  The shell is used by the editor
// scene proxy as the hit-test geometry for mouse-click selection.
//
// Pipeline:
//   1. EstimatePointSpacing() determines an adaptive influence radius
//      proportional to the local density of the point cloud.
//   2. An FGaussianMetaballField is initialised over the full point set.
//   3. FMarchingCubes extracts the iso-surface at DensityThreshold
//      using a seed-continuation strategy seeded at the input points,
//      so only the surface enclosing real geometry is visited.
//   4. The resulting vertices and index buffer are written into OutVertices
//      / OutIndices and later uploaded to the GPU for hit-proxy rendering.
//
// Parameters:
//   TargetResolution        — controls Marching Cubes cube size relative
//                             to the bounding box (higher = finer mesh)
//   InfluenceRadiusScale    — multiplier on EstimatedSpacing for the blob radius
//   DensityThreshold        — iso-value passed to Marching Cubes
//   MaxSeedPoints           — upper bound on continuation seeds (performance cap)
// Returns false if the mesh is empty (degenerate input or all-zero field).
// ============================================================
static bool BuildSelectionShellFromPoints(
    const TArray<FVector3f>& Positions3f,
    const FBox& Bounds,
    int32 TargetResolution,
    float InfluenceRadiusScale,
    float DensityThreshold,
    int32 MaxSeedPoints,
    TArray<FVector3f>& OutVertices,
    TArray<uint32>& OutIndices)
{
    OutVertices.Reset();
    OutIndices.Reset();

    if (Positions3f.Num() < 4)
    {
        return false;
    }

    TargetResolution = FMath::Clamp(TargetResolution, 24, 256);
    InfluenceRadiusScale = FMath::Clamp(InfluenceRadiusScale, 1.0f, 8.0f);
    DensityThreshold = FMath::Clamp(DensityThreshold, 0.01f, 2.0f);
    MaxSeedPoints = FMath::Clamp(MaxSeedPoints, 128, 16384);

    const double EstimatedSpacing = EstimatePointSpacing(Positions3f, Bounds);
    const double MinInfluenceFromBounds = Bounds.GetSize().GetMax() / double(TargetResolution);
    // Influence radius: largest of (spacing * scale), (bounds / resolution), or 10 UU minimum.
    const double InfluenceRadius = FMath::Max3(EstimatedSpacing * (double)InfluenceRadiusScale, MinInfluenceFromBounds, 10.0);
    const double Sigma    = InfluenceRadius / 2.35;   // FWHM → sigma conversion
    const double CellSize = InfluenceRadius;           // One bucket cell per blob radius
    const double CubeSize = FMath::Max3(Bounds.GetSize().GetMax() / double(TargetResolution), EstimatedSpacing * 0.5, 3.0);

    FGaussianMetaballField Field;
    Field.Initialize(Positions3f, Bounds, CellSize, Sigma, InfluenceRadius);

    // Subsample input points to form Marching Cubes continuation seeds.
    TArray<FVector3d> Seeds;
    const int32 SeedStep = FMath::Max(1, Positions3f.Num() / MaxSeedPoints);
    Seeds.Reserve(FMath::DivideAndRoundUp(Positions3f.Num(), SeedStep));
    for (int32 Index = 0; Index < Positions3f.Num(); Index += SeedStep)
    {
        Seeds.Add(FVector3d(Positions3f[Index]));
    }

    FMarchingCubes MarchingCubes;
    MarchingCubes.CubeSize     = CubeSize;
    MarchingCubes.Bounds       = FAxisAlignedBox3d(FVector3d(Bounds.Min), FVector3d(Bounds.Max));
    MarchingCubes.Bounds.Expand(InfluenceRadius * 1.5);  // Pad so blobs near the boundary are captured
    MarchingCubes.IsoValue     = (double)DensityThreshold;
    MarchingCubes.RootMode     = ERootfindingModes::LerpSteps;
    MarchingCubes.RootModeSteps = 3;
    MarchingCubes.bParallelCompute    = true;
    MarchingCubes.bEnableValueCaching = true;
    MarchingCubes.Implicit = [&Field](const FVector3d& Position)
    {
        return Field.Evaluate(Position);
    };

    MarchingCubes.GenerateContinuation(Seeds);
    if (MarchingCubes.Vertices.IsEmpty() || MarchingCubes.Triangles.IsEmpty())
    {
        return false;
    }

    // Convert geometry types to the flat float arrays used by the GPU vertex factory.
    OutVertices.SetNumUninitialized(MarchingCubes.Vertices.Num());
    for (int32 VertexIndex = 0; VertexIndex < MarchingCubes.Vertices.Num(); ++VertexIndex)
    {
        OutVertices[VertexIndex] = FVector3f(MarchingCubes.Vertices[VertexIndex]);
    }

    OutIndices.SetNumUninitialized(MarchingCubes.Triangles.Num() * 3);
    for (int32 TriangleIndex = 0; TriangleIndex < MarchingCubes.Triangles.Num(); ++TriangleIndex)
    {
        const FIndex3i& Triangle = MarchingCubes.Triangles[TriangleIndex];
        OutIndices[TriangleIndex * 3 + 0] = (uint32)Triangle.A;
        OutIndices[TriangleIndex * 3 + 1] = (uint32)Triangle.B;
        OutIndices[TriangleIndex * 3 + 2] = (uint32)Triangle.C;
    }

    return true;
}

static UBodySetup* BuildBodySetupFromConvexHulls(
    UObject* Outer,
    const TArray<FGaussianSplatConvexHull>& InConvexHulls)
{
    if (!Outer || InConvexHulls.IsEmpty())
    {
        return nullptr;
    }

    FKAggregateGeom AggGeom;
    for (const FGaussianSplatConvexHull& Hull : InConvexHulls)
    {
        if (Hull.Vertices.Num() < 4)
        {
            continue;
        }

        FKConvexElem Convex;
        Convex.VertexData.AddUninitialized(Hull.Vertices.Num());
        for (int32 VertexIndex = 0; VertexIndex < Hull.Vertices.Num(); ++VertexIndex)
        {
            Convex.VertexData[VertexIndex] = FVector(Hull.Vertices[VertexIndex]);
        }
        Convex.UpdateElemBox();
        AggGeom.ConvexElems.Add(MoveTemp(Convex));
    }

    if (AggGeom.ConvexElems.IsEmpty())
    {
        return nullptr;
    }

    UBodySetup* NewBodySetup = NewObject<UBodySetup>(Outer, NAME_None, RF_Transient);
    NewBodySetup->AddCollisionFrom(AggGeom);
    return NewBodySetup;
}
} // namespace

// ============================================================
// UGaussianSplatAsset UObject lifecycle
// ============================================================

UGaussianSplatAsset::UGaussianSplatAsset()
{
}

UBodySetup* UGaussianSplatAsset::GetSelectionBodySetup() const
{
    return ShadowProxyMesh ? ShadowProxyMesh->GetBodySetup() : SelectionBodySetup.Get();
}

bool UGaussianSplatAsset::HasSerializedCompressedData() const
{
    if (SerializedCompressedDataVersion != GGaussianSplatCompressedDataVersion)
    {
        return false;
    }

    if (SerializedPackedPositions.IsEmpty()
        || SerializedPackedColors.IsEmpty()
        || SerializedPackedRotations.IsEmpty()
        || SerializedPackedScales.IsEmpty()
        || SerializedPackedNormals.IsEmpty())
    {
        return false;
    }

    if ((SerializedPackedPositions.Num() % 3) != 0)
    {
        return false;
    }

    const int32 DerivedSplatCount = SerializedPackedPositions.Num() / 3;
    if (SerializedPackedColors.Num() != DerivedSplatCount
        || SerializedPackedRotations.Num() != DerivedSplatCount
        || SerializedPackedScales.Num() != DerivedSplatCount
        || SerializedPackedNormals.Num() != DerivedSplatCount)
    {
        return false;
    }

    if (SerializedScaleCodebook.Num() != FGaussianSplatCompressedData::ScaleDummyValueCount)
    {
        return false;
    }

    if (DerivedSplatCount <= 0)
    {
        return false;
    }

    const int32 ExpectedChunkCount = FMath::DivideAndRoundUp(DerivedSplatCount, static_cast<int32>(FGaussianSplatCompressedData::ChunkSize));
    if (SerializedChunkPositionMins.Num() != ExpectedChunkCount
        || SerializedChunkPositionMaxs.Num() != ExpectedChunkCount)
    {
        return false;
    }

    const FVector3f ColorQuantExtent = SerializedColorQuantMax - SerializedColorQuantMin;
    if (ColorQuantExtent.X <= 0.0f || ColorQuantExtent.Y <= 0.0f || ColorQuantExtent.Z <= 0.0f)
    {
        return false;
    }

    if (SerializedSHCoefficientsPerChannel <= 0)
    {
        return SerializedPackedSHData.IsEmpty() && SerializedSHCodebook.IsEmpty();
    }

    return SerializedSHPackedWordsPerSplat > 0
        && SerializedSHCodebook.Num() == FGaussianSplatCompressedData::SHCodebookSize
        && SerializedPackedSHData.Num() == DerivedSplatCount * SerializedSHPackedWordsPerSplat;
}

// ============================================================
// BuildCompressedDataFromRuntimeData
// Converts the temporary float-based import data in SplatData into the compact
// runtime payload used by the renderer. The output is sorted into Morton order,
// split into fixed-size chunks for position quantization, and packed into GPU
// friendly integer streams plus small scalar codebooks for scale and SH.
// ============================================================
bool UGaussianSplatAsset::BuildCompressedDataFromRuntimeData()
{
    double BoundsSeconds = 0.0;
    double SortSeconds = 0.0;
    double ChunkBoundsSeconds = 0.0;
    double ScalePrepSeconds = 0.0;
    double CorePackSeconds = 0.0;
    double SHPackSeconds = 0.0;
    double StatsSeconds = 0.0;

    if (!SplatData.IsValid() || SplatData->SplatCount <= 0)
    {
        CompressedSplatData.Reset();
        return false;
    }

    // Validate that every required source stream has the expected per-splat
    // layout before allocating the compressed payload.
    const int32 NumSplats = SplatData->SplatCount;
    const int32 SHCoeffCount = GetSHCoeffCountForDegree(SplatData->SHDegree);
    if (SplatData->Positions.Num() != NumSplats * 3
        || SplatData->Colors.Num() != NumSplats * 4
        || SplatData->Normals.Num() != NumSplats * 3
        || SplatData->LogScales.Num() != NumSplats * 3
        || SplatData->RotationQuats.Num() != NumSplats * 4)
    {
        CompressedSplatData.Reset();
        return false;
    }

    TSharedPtr<FGaussianSplatCompressedData> NewCompressed = MakeShared<FGaussianSplatCompressedData>();
    NewCompressed->SplatCount = NumSplats;
    NewCompressed->SHDegree = SplatData->SHDegree;
    NewCompressed->SHCoefficientsPerChannel = SHCoeffCount;
    NewCompressed->SHPackedWordsPerSplat = GetSHPackedWordsPerSplat(SplatData->SHDegree);
    NewCompressed->PackedPositions.SetNumUninitialized(NumSplats * 3);
    NewCompressed->PackedColors.SetNumUninitialized(NumSplats);
    NewCompressed->PackedRotations.SetNumUninitialized(NumSplats);
    NewCompressed->PackedScales.SetNumUninitialized(NumSplats);
    NewCompressed->PackedNormals.SetNumUninitialized(NumSplats);
    NewCompressed->ScaleCodebook.SetNumZeroed(FGaussianSplatCompressedData::ScaleDummyValueCount);

    const int32 ChunkCount = NewCompressed->GetChunkCount();
    NewCompressed->ChunkPositionMins.SetNumUninitialized(ChunkCount);
    NewCompressed->ChunkPositionMaxs.SetNumUninitialized(ChunkCount);

    const float* Positions = SplatData->Positions.GetData();
    const float* Colors = SplatData->Colors.GetData();
    const float* Normals = SplatData->Normals.GetData();
    const float* LogScales = SplatData->LogScales.GetData();
    const float* Quats = SplatData->RotationQuats.GetData();
    const float* SHDataForStats = nullptr;

    FVector3f BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
    FVector3f BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    FVector3f ColorMin(FLT_MAX, FLT_MAX, FLT_MAX);
    FVector3f ColorMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    {
        // One linear pass gathers asset bounds and RGB quantization ranges.
        FScopedDurationTimer BoundsTimer(BoundsSeconds);
        for (int32 SplatIndex = 0; SplatIndex < NumSplats; ++SplatIndex)
        {
            const FVector3f Position(
                Positions[SplatIndex * 3 + 0],
                Positions[SplatIndex * 3 + 1],
                Positions[SplatIndex * 3 + 2]);
            BoundsMin.X = FMath::Min(BoundsMin.X, Position.X);
            BoundsMin.Y = FMath::Min(BoundsMin.Y, Position.Y);
            BoundsMin.Z = FMath::Min(BoundsMin.Z, Position.Z);
            BoundsMax.X = FMath::Max(BoundsMax.X, Position.X);
            BoundsMax.Y = FMath::Max(BoundsMax.Y, Position.Y);
            BoundsMax.Z = FMath::Max(BoundsMax.Z, Position.Z);

            ColorMin.X = FMath::Min(ColorMin.X, Colors[SplatIndex * 4 + 0]);
            ColorMin.Y = FMath::Min(ColorMin.Y, Colors[SplatIndex * 4 + 1]);
            ColorMin.Z = FMath::Min(ColorMin.Z, Colors[SplatIndex * 4 + 2]);
            ColorMax.X = FMath::Max(ColorMax.X, Colors[SplatIndex * 4 + 0]);
            ColorMax.Y = FMath::Max(ColorMax.Y, Colors[SplatIndex * 4 + 1]);
            ColorMax.Z = FMath::Max(ColorMax.Z, Colors[SplatIndex * 4 + 2]);
        }
    }

    TArray<int32> SortedIndices;
    const FVector3f BoundsExtent = BoundsMax - BoundsMin;
    {
        // Morton order improves spatial locality so each fixed-size chunk has
        // a tight local bounds range for 16-bit position quantization.
        FScopedDurationTimer SortTimer(SortSeconds);
        BuildMortonSortedIndices(Positions, NumSplats, BoundsMin, BoundsExtent, SortedIndices);
    }

    {
        // Compute per-chunk position min/max after Morton sorting. Positions are
        // stored as normalized uint16 values inside these chunk-local bounds.
        FScopedDurationTimer ChunkBoundsTimer(ChunkBoundsSeconds);
        for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
        {
            const int32 ChunkStart = ChunkIndex * (int32)FGaussianSplatCompressedData::ChunkSize;
            const int32 ChunkEnd = FMath::Min(ChunkStart + (int32)FGaussianSplatCompressedData::ChunkSize, NumSplats);

            FVector3f PosMin(FLT_MAX, FLT_MAX, FLT_MAX);
            FVector3f PosMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

            for (int32 SplatIndex = ChunkStart; SplatIndex < ChunkEnd; ++SplatIndex)
            {
                const int32 SourceIndex = SortedIndices[SplatIndex];
                const FVector3f Position(
                    Positions[SourceIndex * 3 + 0],
                    Positions[SourceIndex * 3 + 1],
                    Positions[SourceIndex * 3 + 2]);

                PosMin.X = FMath::Min(PosMin.X, Position.X);
                PosMin.Y = FMath::Min(PosMin.Y, Position.Y);
                PosMin.Z = FMath::Min(PosMin.Z, Position.Z);
                PosMax.X = FMath::Max(PosMax.X, Position.X);
                PosMax.Y = FMath::Max(PosMax.Y, Position.Y);
                PosMax.Z = FMath::Max(PosMax.Z, Position.Z);
            }

            NewCompressed->ChunkPositionMins[ChunkIndex] = FVector4f(PosMin, 0.0f);
            NewCompressed->ChunkPositionMaxs[ChunkIndex] = FVector4f(PosMax, 0.0f);
        }
    }

    NewCompressed->BoundsMin = BoundsMin;
    NewCompressed->BoundsMax = BoundsMax;

    if (FMath::Abs(ColorMax.X - ColorMin.X) < UE_SMALL_NUMBER) ColorMax.X = ColorMin.X + 1.0f;
    if (FMath::Abs(ColorMax.Y - ColorMin.Y) < UE_SMALL_NUMBER) ColorMax.Y = ColorMin.Y + 1.0f;
    if (FMath::Abs(ColorMax.Z - ColorMin.Z) < UE_SMALL_NUMBER) ColorMax.Z = ColorMin.Z + 1.0f;
    NewCompressed->ColorQuantMin = ColorMin;
    NewCompressed->ColorQuantMax = ColorMax;

    {
        // Keep a tiny dummy scale payload for legacy SRV binding. Actual scale
        // values are packed directly into PackedScales with fixed log quantization.
        FScopedDurationTimer ScalePrepTimer(ScalePrepSeconds);
        NewCompressed->ScaleCodebook[0] = 0.0f;
    }

    {
        // Pack the core per-splat attributes in Morton order. This stream is what
        // the renderer consumes directly: positions, scales, rotations, color,
        // opacity and packed normals kept for future normal-aware extensions.
        FScopedDurationTimer CorePackTimer(CorePackSeconds);
        for (int32 SplatIndex = 0; SplatIndex < NumSplats; ++SplatIndex)
        {
            const int32 SourceIndex = SortedIndices[SplatIndex];
            const int32 ChunkIndex = SplatIndex / (int32)FGaussianSplatCompressedData::ChunkSize;
            const FVector3f Position(
                Positions[SourceIndex * 3 + 0],
                Positions[SourceIndex * 3 + 1],
                Positions[SourceIndex * 3 + 2]);
            const FVector3f LogScale(
                LogScales[SourceIndex * 3 + 0],
                LogScales[SourceIndex * 3 + 1],
                LogScales[SourceIndex * 3 + 2]);

            const FVector3f ChunkPosMin = FVector3f(NewCompressed->ChunkPositionMins[ChunkIndex]);
            const FVector3f ChunkPosMax = FVector3f(NewCompressed->ChunkPositionMaxs[ChunkIndex]);

            const FVector3f PosExtent = ChunkPosMax - ChunkPosMin;
            const FVector3f PositionNormalized(
                PosExtent.X > UE_SMALL_NUMBER ? (Position.X - ChunkPosMin.X) / PosExtent.X : 0.5f,
                PosExtent.Y > UE_SMALL_NUMBER ? (Position.Y - ChunkPosMin.Y) / PosExtent.Y : 0.5f,
                PosExtent.Z > UE_SMALL_NUMBER ? (Position.Z - ChunkPosMin.Z) / PosExtent.Z : 0.5f);

            // Position is chunk-local 16-bit UNORM per axis.
            PackPosition_16_16_16(PositionNormalized, NewCompressed->PackedPositions.GetData() + SplatIndex * 3);

            // Scale stores three direct 8-bit fixed log-scale values.
            NewCompressed->PackedScales[SplatIndex] = PackFixedLogScale_8_8_8(LogScale);

            // Imported rotations are stored as UE-space WXYZ quaternions;
            // FQuat4f expects XYZW.
            const FQuat4f RotationQuat(
                Quats[SourceIndex * 4 + 1],
                Quats[SourceIndex * 4 + 2],
                Quats[SourceIndex * 4 + 3],
                Quats[SourceIndex * 4 + 0]);
            NewCompressed->PackedRotations[SplatIndex] = PackSmallestThreeQuaternion(RotationQuat);

            // Base color uses per-asset RGB min/max plus direct 8-bit opacity.
            const uint32 R = QuantizeUnorm((Colors[SourceIndex * 4 + 0] - ColorMin.X) / (ColorMax.X - ColorMin.X), 255u);
            const uint32 G = QuantizeUnorm((Colors[SourceIndex * 4 + 1] - ColorMin.Y) / (ColorMax.Y - ColorMin.Y), 255u);
            const uint32 B = QuantizeUnorm((Colors[SourceIndex * 4 + 2] - ColorMin.Z) / (ColorMax.Z - ColorMin.Z), 255u);
            const uint32 A = QuantizeUnorm(Colors[SourceIndex * 4 + 3], 255u);
            NewCompressed->PackedColors[SplatIndex] = R | (G << 8u) | (B << 16u) | (A << 24u);

            NewCompressed->PackedNormals[SplatIndex] = PackNormal(
                FVector3f(
                    Normals[SourceIndex * 3 + 0],
                    Normals[SourceIndex * 3 + 1],
                    Normals[SourceIndex * 3 + 2]));
        }
    }

    {
        // Higher-order SH is optional. DC color is already packed above; this
        // stream stores only the view-dependent SH coefficients.
        FScopedDurationTimer SHPackTimer(SHPackSeconds);
        if (SHCoeffCount > 0 && SplatData->SphericalHarmonics.Num() == NumSplats * SHCoeffCount * 3)
        {
            const float* SHData = SplatData->SphericalHarmonics.GetData();
            SHDataForStats = SHData;
            float SHMin = FLT_MAX;
            float SHMax = -FLT_MAX;
            for (int32 Index = 0; Index < SplatData->SphericalHarmonics.Num(); ++Index)
            {
                SHMin = FMath::Min(SHMin, SHData[Index]);
                SHMax = FMath::Max(SHMax, SHData[Index]);
            }

            if (FMath::Abs(SHMax - SHMin) < UE_SMALL_NUMBER)
            {
                SHMax = SHMin + 1.0f;
            }

            if (!BuildDirectSHStreamFromSource(
                SHData,
                SortedIndices,
                NumSplats,
                SHCoeffCount * 3,
                SHMin,
                SHMax,
                NewCompressed->PackedSHData,
                NewCompressed->SHCodebook))
            {
                CompressedSplatData.Reset();
                return false;
            }
        }
    }

    {
        // Keep diagnostics in one place so build functions remain focused on
        // producing compressed streams.
        FScopedDurationTimer StatsTimer(StatsSeconds);
        LogCompressedAttributeStats(this, *NewCompressed, LogScales, SHDataForStats, SortedIndices);
    }

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatting: BuildCompressedData timings for %s: bounds %.2f ms, morton sort %.2f ms, chunk ranges %.2f ms, scale prep %.2f ms, core pack %.2f ms, SH pack %.2f ms, stats %.2f ms."),
        *GetPathName(),
        BoundsSeconds * 1000.0,
        SortSeconds * 1000.0,
        ChunkBoundsSeconds * 1000.0,
        ScalePrepSeconds * 1000.0,
        CorePackSeconds * 1000.0,
        SHPackSeconds * 1000.0,
        StatsSeconds * 1000.0);

    CompressedSplatData = MoveTemp(NewCompressed);
    return true;
}

void UGaussianSplatAsset::CacheSerializedCompressedDataFromRuntimeData()
{
    if (!CompressedSplatData.IsValid() || !CompressedSplatData->IsValid())
    {
        SerializedPackedPositions.Reset();
        SerializedPackedColors.Reset();
        SerializedPackedRotations.Reset();
        SerializedPackedScales.Reset();
        SerializedPackedNormals.Reset();
        SerializedScaleCodebook.Reset();
        SerializedSHCodebook.Reset();
        SerializedPackedSHData.Reset();
        SerializedChunkPositionMins.Reset();
        SerializedChunkPositionMaxs.Reset();
        SerializedBoundsMin = FVector3f::ZeroVector;
        SerializedBoundsMax = FVector3f::ZeroVector;
        SerializedColorQuantMin = FVector3f::ZeroVector;
        SerializedColorQuantMax = FVector3f(1.0f, 1.0f, 1.0f);
        SerializedSHCoefficientsPerChannel = 0;
        SerializedSHPackedWordsPerSplat = 0;
        SerializedCompressedDataVersion = 0;
        return;
    }

    SplatCount = CompressedSplatData->SplatCount;
    SHDegree = CompressedSplatData->SHDegree;
    SerializedPackedPositions = CompressedSplatData->PackedPositions;
    SerializedPackedColors = CompressedSplatData->PackedColors;
    SerializedPackedRotations = CompressedSplatData->PackedRotations;
    SerializedPackedScales = CompressedSplatData->PackedScales;
    SerializedPackedNormals = CompressedSplatData->PackedNormals;
    SerializedScaleCodebook = CompressedSplatData->ScaleCodebook;
    SerializedSHCodebook = CompressedSplatData->SHCodebook;
    SerializedPackedSHData = CompressedSplatData->PackedSHData;
    SerializedChunkPositionMins = CompressedSplatData->ChunkPositionMins;
    SerializedChunkPositionMaxs = CompressedSplatData->ChunkPositionMaxs;
    SerializedBoundsMin = CompressedSplatData->BoundsMin;
    SerializedBoundsMax = CompressedSplatData->BoundsMax;
    SerializedColorQuantMin = CompressedSplatData->ColorQuantMin;
    SerializedColorQuantMax = CompressedSplatData->ColorQuantMax;
    SerializedSHCoefficientsPerChannel = CompressedSplatData->SHCoefficientsPerChannel;
    SerializedSHPackedWordsPerSplat = CompressedSplatData->SHPackedWordsPerSplat;
    SerializedCompressedDataVersion = GGaussianSplatCompressedDataVersion;
}

bool UGaussianSplatAsset::RestoreCompressedRuntimeDataFromSerializedState()
{
    if (!HasSerializedCompressedData())
    {
        return false;
    }

    TSharedPtr<FGaussianSplatCompressedData> NewCompressed = MakeShared<FGaussianSplatCompressedData>();
    NewCompressed->PackedPositions = SerializedPackedPositions;
    NewCompressed->PackedColors = SerializedPackedColors;
    NewCompressed->PackedRotations = SerializedPackedRotations;
    NewCompressed->PackedScales = SerializedPackedScales;
    NewCompressed->PackedNormals = SerializedPackedNormals;
    NewCompressed->ScaleCodebook = SerializedScaleCodebook;
    NewCompressed->SHCodebook = SerializedSHCodebook;
    NewCompressed->PackedSHData = SerializedPackedSHData;
    NewCompressed->ChunkPositionMins = SerializedChunkPositionMins;
    NewCompressed->ChunkPositionMaxs = SerializedChunkPositionMaxs;
    NewCompressed->BoundsMin = SerializedBoundsMin;
    NewCompressed->BoundsMax = SerializedBoundsMax;
    NewCompressed->ColorQuantMin = SerializedColorQuantMin;
    NewCompressed->ColorQuantMax = SerializedColorQuantMax;
    NewCompressed->SplatCount = SerializedPackedPositions.Num() / 3;
    NewCompressed->SHDegree = SHDegree;
    NewCompressed->SHCoefficientsPerChannel = SerializedSHCoefficientsPerChannel;
    NewCompressed->SHPackedWordsPerSplat = SerializedSHPackedWordsPerSplat;
    CompressedSplatData = MoveTemp(NewCompressed);
    SplatCount = CompressedSplatData->SplatCount;
    return true;
}

void UGaussianSplatAsset::PostLoad()
{
    Super::PostLoad();

    RestoreCompressedRuntimeDataFromSerializedState();

    if (!IsLoaded())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("GaussianSplatAsset: Asset %s is missing compressed render data. Use the explicit reimport/import action to rebuild it from %s."),
            *GetPathName(),
            SourcePlyPath.IsEmpty() ? TEXT("<empty SourcePlyPath>") : *SourcePlyPath);
    }

    if (IsLoaded() && (SelectionMeshVertices.IsEmpty() || SelectionMeshIndices.IsEmpty() || !SelectionBodySetup))
    {
        RebuildSelectionGeometry();
    }
}

// ============================================================
// RebuildSelectionGeometry
// (Re-)builds collision and selection geometry from
// the current SplatData.  Called after a PLY is loaded and whenever
// asset properties that affect the selection shape are modified.
//
// Produces two complementary outputs:
//   ConvexHulls            - compact physics hulls used by UBodySetup
//   SelectionMeshVertices /
//   SelectionMeshIndices   - smooth voxel-shell mesh (or convex fallback)
//                            rendered by the scene proxy for hit-testing and
//                            used as the fallback shadow-caster mesh
// ============================================================
void UGaussianSplatAsset::RebuildSelectionGeometry()
{
    ConvexHulls.Reset();
    SelectionMeshVertices.Reset();
    SelectionMeshIndices.Reset();
    SelectionBodySetup = nullptr;

    if (!IsLoaded())
    {
        return;
    }

    if (ShadowProxyMesh)
    {
        UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: Using shared shadow proxy mesh %s for asset %s; generated selection geometry is skipped."),
            *GetPathNameSafe(ShadowProxyMesh),
            *GetPathName());
        return;
    }

    TArray<FVector3f> Positions3f;
    Positions3f.SetNumUninitialized(CompressedSplatData->SplatCount);
    FBox Bounds(ForceInit);
    for (int32 i = 0; i < CompressedSplatData->SplatCount; ++i)
    {
        const FVector3f Position = DecodePackedPosition(*CompressedSplatData, i);
        Positions3f[i] = Position;
        Bounds += FVector(Position);
    }

    // Build the multi-convex approximation first; it always produces a valid fallback mesh.
    TArray<FVector3f> FallbackSelectionVertices;
    TArray<uint32> FallbackSelectionIndices;
    BuildMultiConvexApproximation(
        Positions3f,
        Bounds,
        MaxSelectionConvexHulls,
        ConvexHulls,
        FallbackSelectionVertices,
        FallbackSelectionIndices);

    // Optionally replace the convex-hull fallback mesh with a smoother voxel shell.
    bool bBuiltShell = false;
    if (bUseVoxelSelectionShell)
    {
        bBuiltShell = BuildSelectionShellFromPoints(
            Positions3f,
            Bounds,
            SelectionShellTargetResolution,
            SelectionShellInfluenceRadiusScale,
            SelectionShellDensityThreshold,
            SelectionShellMaxSeedPoints,
            SelectionMeshVertices,
            SelectionMeshIndices);
    }

    if (!bBuiltShell)
    {
        SelectionMeshVertices = MoveTemp(FallbackSelectionVertices);
        SelectionMeshIndices  = MoveTemp(FallbackSelectionIndices);
    }

    SelectionBodySetup = BuildBodySetupFromConvexHulls(this, ConvexHulls);

    UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: Built %d convex hulls and a %s selection mesh (%d verts, %d tris)."),
        ConvexHulls.Num(),
        bBuiltShell ? TEXT("voxel shell") : TEXT("convex fallback"),
        SelectionMeshVertices.Num(), SelectionMeshIndices.Num() / 3);
}

#if WITH_EDITOR

// ============================================================
// PostEditChangeProperty
// Triggers a selection-geometry rebuild whenever the user edits any
// property that controls the shape or quality of the convex hulls or
// the voxel selection shell.  Also notifies all live UGaussianSplatComponent
// instances referencing this asset so they rebuild their BodySetup and
// invalidate their scene proxy.
// ============================================================
void UGaussianSplatAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName PropertyName = PropertyChangedEvent.GetPropertyName();
    const bool bSelectionShapeChanged =
        PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, ShadowProxyMesh)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, bUseVoxelSelectionShell)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, SelectionShellTargetResolution)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, SelectionShellInfluenceRadiusScale)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, SelectionShellDensityThreshold)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, SelectionShellMaxSeedPoints)
        || PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, MaxSelectionConvexHulls);


    if (bSelectionShapeChanged)
    {
        RebuildSelectionGeometry();

        // Propagate the change to all components currently using this asset.
        for (TObjectIterator<UGaussianSplatComponent> It; It; ++It)
        {
            UGaussianSplatComponent* Component = *It;
            if (IsValid(Component) && Component->GaussianSplatAsset == this)
            {
                Component->OnSplatAssetChanged();
            }
        }
    }
}
#endif

// ============================================================
// LoadFromPly
// Primary entry point for importing a .ply file into this asset.
// Delegates the actual file parsing to FGaussianSplatPlyLoader,
// then caches top-level metadata (SplatCount, SHDegree) for Blueprint
// and UI access, and rebuilds the fallback selection/shadow geometry
// so the asset is ready immediately.
// ============================================================
bool UGaussianSplatAsset::LoadFromPly(const FString& FilePath)
{
    SourcePlyPath = FilePath;

    auto NewData = MakeShared<FGaussianSplatData>();
    double LoaderSeconds = 0.0;
    double CompressSeconds = 0.0;
    double SerializeSeconds = 0.0;
    double SelectionGeometrySeconds = 0.0;

#if WITH_EDITOR
    FScopedSlowTask ImportSlowTask(
        4.0f,
        FText::Format(
            NSLOCTEXT("GaussianSplatting", "ImportPlyProgress", "Importing Gaussian splat from {0}"),
            FText::FromString(FPaths::GetCleanFilename(FilePath))));
    ImportSlowTask.MakeDialog();
#endif

    {
#if WITH_EDITOR
        ImportSlowTask.EnterProgressFrame(
            1.0f,
            NSLOCTEXT("GaussianSplatting", "ImportPlyLoadStep", "Reading PLY data"));
#endif
        FScopedDurationTimer LoaderTimer(LoaderSeconds);
        if (!FGaussianSplatPlyLoader::Load(FilePath, *NewData))
        {
            UE_LOG(LogTemp, Error, TEXT("GaussianSplatAsset: Failed to load PLY: %s"), *FilePath);
            return false;
        }
    }

    SplatData  = NewData;
    SplatCount = NewData->SplatCount;
    SHDegree   = NewData->SHDegree;

    {
#if WITH_EDITOR
        ImportSlowTask.EnterProgressFrame(
            1.0f,
            NSLOCTEXT("GaussianSplatting", "ImportPlyCompressStep", "Building compressed render data"));
#endif
        FScopedDurationTimer CompressTimer(CompressSeconds);
        if (!BuildCompressedDataFromRuntimeData())
        {
            UE_LOG(LogTemp, Error, TEXT("GaussianSplatAsset: Failed to build compressed render data for %s"), *FilePath);
            return false;
        }
    }

    {
#if WITH_EDITOR
        ImportSlowTask.EnterProgressFrame(
            1.0f,
            NSLOCTEXT("GaussianSplatting", "ImportPlySerializeStep", "Caching serialized compressed data"));
#endif
        FScopedDurationTimer SerializeTimer(SerializeSeconds);
        CacheSerializedCompressedDataFromRuntimeData();
    }

    {
#if WITH_EDITOR
        ImportSlowTask.EnterProgressFrame(
            1.0f,
            NSLOCTEXT("GaussianSplatting", "ImportPlySelectionStep", "Building selection and shadow proxy geometry"));
#endif
        FScopedDurationTimer SelectionGeometryTimer(SelectionGeometrySeconds);
        RebuildSelectionGeometry();
    }

    SplatData.Reset();

    UE_LOG(LogTemp, Log,
        TEXT("GaussianSplatAsset: Imported %d splats (SH degree %d) from %s in %.2f ms (load %.2f ms, compress %.2f ms, serialize %.2f ms, selection geometry %.2f ms)"),
        SplatCount,
        SHDegree,
        *FilePath,
        (LoaderSeconds + CompressSeconds + SerializeSeconds + SelectionGeometrySeconds) * 1000.0,
        LoaderSeconds * 1000.0,
        CompressSeconds * 1000.0,
        SerializeSeconds * 1000.0,
        SelectionGeometrySeconds * 1000.0);

    return true;
}

// ============================================================
// GetBounds
// Computes the tight axis-aligned bounding box of all splat positions.
// Returns an empty (invalid) FBox if the asset has not been loaded.
// ============================================================
FBox UGaussianSplatAsset::GetBounds() const
{
    if (!IsLoaded())
    {
        return FBox(ForceInit);
    }

    FBox Bounds(ForceInit);
    Bounds += FVector(CompressedSplatData->BoundsMin);
    Bounds += FVector(CompressedSplatData->BoundsMax);
    return Bounds;
}
