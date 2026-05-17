#include "GaussianSplatPlyLoader.h"
#include "Async/ParallelFor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Math/RotationMatrix.h"
#include "Math/UnrealMathUtility.h"
#include "ProfilingDebugging/ScopedTimers.h"

// ============================================================
// PLY file loading for 3D Gaussian Splatting
// ============================================================

namespace
{
static constexpr float SourceToUEUnitScale = 100.0f;
static constexpr float SourceToUELogScaleOffset = 4.605170185988091368f; // ln(100)

// ============================================================
// Quaternion-to-matrix helper
// Builds a 3x3 rotation matrix from a WXYZ quaternion.
// ============================================================
static void BuildRotationMatrixFromQuat(const float* quat, float OutRotation[3][3])
{
    float qw = quat[0];
    float qx = quat[1];
    float qy = quat[2];
    float qz = quat[3];

    const float QuaternionLength = FMath::Sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
    if (QuaternionLength > 0.0001f)
    {
        qw /= QuaternionLength;
        qx /= QuaternionLength;
        qy /= QuaternionLength;
        qz /= QuaternionLength;
    }

    OutRotation[0][0] = 1.0f - 2.0f * (qy * qy + qz * qz);
    OutRotation[0][1] = 2.0f * (qx * qy - qw * qz);
    OutRotation[0][2] = 2.0f * (qx * qz + qw * qy);
    OutRotation[1][0] = 2.0f * (qx * qy + qw * qz);
    OutRotation[1][1] = 1.0f - 2.0f * (qx * qx + qz * qz);
    OutRotation[1][2] = 2.0f * (qy * qz - qw * qx);
    OutRotation[2][0] = 2.0f * (qx * qz - qw * qy);
    OutRotation[2][1] = 2.0f * (qy * qz + qw * qx);
    OutRotation[2][2] = 1.0f - 2.0f * (qx * qx + qy * qy);
}

// ============================================================
// Source direction to UE direction
// Applies the project's fixed source(3DGS)->UE axis mapping to a direction.
// ============================================================
static FVector3f ConvertSourceDirectionToUE(const FVector3f& SourceDirection)
{
    return FVector3f(SourceDirection.Z, SourceDirection.X, -SourceDirection.Y);
}

// ============================================================
// Source quaternion to UE quaternion
// Converts a source-space WXYZ quaternion into a UE-space WXYZ quaternion.
// The source->UE basis mapping includes a reflection, so we rebuild a proper
// UE rotation from the converted local X/Y basis vectors.
// ============================================================
static void ConvertSourceQuatToUEWXYZ(const float* SourceQuatWXYZ, float* OutUEQuatWXYZ)
{
    float SourceRotation[3][3];
    BuildRotationMatrixFromQuat(SourceQuatWXYZ, SourceRotation);

    const FVector3f UEAxisX3f = ConvertSourceDirectionToUE(FVector3f(
        SourceRotation[0][0],
        SourceRotation[1][0],
        SourceRotation[2][0])).GetSafeNormal();
    const FVector3f UEAxisY3f = ConvertSourceDirectionToUE(FVector3f(
        SourceRotation[0][1],
        SourceRotation[1][1],
        SourceRotation[2][1])).GetSafeNormal();
    const FVector UEAxisX(UEAxisX3f.X, UEAxisX3f.Y, UEAxisX3f.Z);
    const FVector UEAxisY(UEAxisY3f.X, UEAxisY3f.Y, UEAxisY3f.Z);

    FQuat UEQuat = FRotationMatrix::MakeFromXY(UEAxisX, UEAxisY).ToQuat();
    UEQuat.Normalize();

    OutUEQuatWXYZ[0] = UEQuat.W;
    OutUEQuatWXYZ[1] = UEQuat.X;
    OutUEQuatWXYZ[2] = UEQuat.Y;
    OutUEQuatWXYZ[3] = UEQuat.Z;
}

static FVector3f EstimateNormalFromGaussianShape(const float* UEQuatWXYZ, const float* LogScale)
{
    int32 MinScaleAxis = 0;
    if (LogScale[1] < LogScale[MinScaleAxis])
    {
        MinScaleAxis = 1;
    }
    if (LogScale[2] < LogScale[MinScaleAxis])
    {
        MinScaleAxis = 2;
    }

    const FVector3f LocalAxis =
        MinScaleAxis == 0 ? FVector3f(1.0f, 0.0f, 0.0f) :
        MinScaleAxis == 1 ? FVector3f(0.0f, 1.0f, 0.0f) :
                            FVector3f(0.0f, 0.0f, 1.0f);

    const FQuat4f UEQuat(
        UEQuatWXYZ[1],
        UEQuatWXYZ[2],
        UEQuatWXYZ[3],
        UEQuatWXYZ[0]);

    return UEQuat.RotateVector(LocalAxis).GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0.0f, 0.0f, 1.0f));
}

}


// TODO: stream reading
bool FGaussianSplatPlyLoader::Load(const FString& FilePath, FGaussianSplatData& OutData)
{
    UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: Loading PLY file: %s"), *FilePath);
    double FileReadSeconds = 0.0;
    double HeaderParseSeconds = 0.0;
    double VertexDecodeSeconds = 0.0;

    // Read the entire file
    TArray64<uint8> FileData;
    {
        FScopedDurationTimer FileReadTimer(FileReadSeconds);
        if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
        {
            UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: Failed to open file: %s"), *FilePath);
            return false;
        }
    }

    UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: File size: %lld bytes"), FileData.Num());

    // Parse the PLY header
    int64      DataOffset = 0;
    FPlyHeader Header;
    {
        FScopedDurationTimer HeaderParseTimer(HeaderParseSeconds);
        if (!ParseHeader(FileData, DataOffset, Header))
        {
            UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: Failed to parse PLY header"));
            return false;
        }
    }

    UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: Found %d splats, %d SH rest coeffs"),
        Header.NumVertices, Header.NumSHRestCoeffs);

    // Validate required properties
    if (Header.Offset_x < 0 || Header.Offset_y < 0 || Header.Offset_z < 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: Missing position properties in PLY"));
        return false;
    }

    const int32 N = Header.NumVertices;
    OutData.SplatCount = N;

    // Determine SH degree from number of rest coefficients
    // SH rest coefficients per channel: 3 (deg1), 8 (deg2), 15 (deg3)
    // Total rest floats: 9 (deg1), 24 (deg2), 45 (deg3)
    OutData.SHDegree = 0;
    if (Header.NumSHRestCoeffs >= 9)  OutData.SHDegree = 1;
    if (Header.NumSHRestCoeffs >= 24) OutData.SHDegree = 2;
    if (Header.NumSHRestCoeffs >= 45) OutData.SHDegree = 3;

    // Allocate output arrays
    OutData.Positions.SetNum(N * 3);
    OutData.Colors.SetNum(N * 4);
    OutData.Normals.SetNumZeroed(N * 3);
    OutData.LogScales.SetNum(N * 3);
    OutData.RotationQuats.SetNum(N * 4);

    // SH buffer: degrees 1-3 only (DC is in Colors)
    // For each splat: SHCoeffCount * 3 channels
    // SHCoeffCount: 3 (deg1), 8 (deg2), 15 (deg3)
    int32 SHCoeffCount = 0;
    if (OutData.SHDegree >= 3) SHCoeffCount = 15;
    else if (OutData.SHDegree >= 2) SHCoeffCount = 8;
    else if (OutData.SHDegree >= 1) SHCoeffCount = 3;
    OutData.SphericalHarmonics.SetNumZeroed(N * SHCoeffCount * 3);

    const uint8* DataPtr = FileData.GetData() + DataOffset;

    // Process each splat
    {
        FScopedDurationTimer VertexDecodeTimer(VertexDecodeSeconds);
        ParallelFor(N, [&](int32 i)
        {
        const uint8* Vertex = DataPtr + (int64)i * Header.BytesPerVertex;

        auto ReadFloat = [&](int32 offset) -> float
        {
            if (offset < 0 || offset + 4 > Header.BytesPerVertex)
                return 0.0f;
            float val;
            FMemory::Memcpy(&val, Vertex + offset, 4);
            return val;
        };

        // Position
        float px = ReadFloat(Header.Offset_x);
        float py = ReadFloat(Header.Offset_y);
        float pz = ReadFloat(Header.Offset_z);
        // Coordinate system conversion: 3DGS/COLMAP (RDF) to UE convention.
        // Mapping: UE.X = 3DGS.Z, UE.Y = 3DGS.X, UE.Z = -3DGS.Y.
        // Additionally, scale from meters (3DGS) to centimeters (UE default unit).
        OutData.Positions[i * 3 + 0] =  pz * SourceToUEUnitScale;
        OutData.Positions[i * 3 + 1] =  px * SourceToUEUnitScale;
        OutData.Positions[i * 3 + 2] = -py * SourceToUEUnitScale;

        // Scale (stored as log), in meters
        float logScale[3];
        logScale[0] = ReadFloat(Header.Offset_scale[0]) + SourceToUELogScaleOffset;
        logScale[1] = ReadFloat(Header.Offset_scale[1]) + SourceToUELogScaleOffset;
        logScale[2] = ReadFloat(Header.Offset_scale[2]) + SourceToUELogScaleOffset;
        OutData.LogScales[i * 3 + 0] = logScale[0];
        OutData.LogScales[i * 3 + 1] = logScale[1];
        OutData.LogScales[i * 3 + 2] = logScale[2];

        // Rotation quaternion (stored as rot_0=w, rot_1=x, rot_2=y, rot_3=z in .ply)
        float quat[4];
        quat[0] = ReadFloat(Header.Offset_rot[0]);  // w
        quat[1] = ReadFloat(Header.Offset_rot[1]);  // x
        quat[2] = ReadFloat(Header.Offset_rot[2]);  // y
        quat[3] = ReadFloat(Header.Offset_rot[3]);  // z
        float UEQuat[4];
        ConvertSourceQuatToUEWXYZ(quat, UEQuat);
        OutData.RotationQuats[i * 4 + 0] = UEQuat[0];
        OutData.RotationQuats[i * 4 + 1] = UEQuat[1];
        OutData.RotationQuats[i * 4 + 2] = UEQuat[2];
        OutData.RotationQuats[i * 4 + 3] = UEQuat[3];

        const FVector3f EstimatedNormal = EstimateNormalFromGaussianShape(UEQuat, logScale);
        OutData.Normals[i * 3 + 0] = EstimatedNormal.X;
        OutData.Normals[i * 3 + 1] = EstimatedNormal.Y;
        OutData.Normals[i * 3 + 2] = EstimatedNormal.Z;

        // DC color component (SH degree 0)
        // stored as linear color value (not gamma-corrected)
        // The SH C0 constant (0.28209479...) is applied to the DC component
        // Color = SH_C0 * f_dc + 0.5 (offset to center the sigmoid output)
        float dc_r = ReadFloat(Header.Offset_fdc[0]);
        float dc_g = ReadFloat(Header.Offset_fdc[1]);
        float dc_b = ReadFloat(Header.Offset_fdc[2]);
        
        // Keep the DC term in the same linear domain as the original 3DGS renderer.
        // Do not clamp here: higher-order SH can legitimately push the final radiance
        // above 1.0, and the reference path only clamps the final summed color to >= 0.
        static const float SH_C0 = 0.28209479177387814f;
        float linear_r = SH_C0 * dc_r + 0.5f;
        float linear_g = SH_C0 * dc_g + 0.5f;
        float linear_b = SH_C0 * dc_b + 0.5f;

        // Opacity (stored as logit, apply sigmoid)
        float logit_opacity = ReadFloat(Header.Offset_opacity);
        float opacity = Sigmoid(logit_opacity);

        OutData.Colors[i * 4 + 0] = linear_r;
        OutData.Colors[i * 4 + 1] = linear_g;
        OutData.Colors[i * 4 + 2] = linear_b;
        OutData.Colors[i * 4 + 3] = opacity;

        // SH higher-degree coefficients
        // PLY layout (for degree 3 / 45 total f_rest properties):
        //   f_rest_0  .. f_rest_14  = R channel, 15 coefficients
        //   f_rest_15 .. f_rest_29  = G channel, 15 coefficients
        //   f_rest_30 .. f_rest_44  = B channel, 15 coefficients
        // Header.NumSHRestCoeffs is the TOTAL number of f_rest properties (9, 24, or 45).
        // The per-channel count is NumSHRestCoeffs / 3.
        //
        // We repack into interleaved (R,G,B) per coefficient:
        //   SH[base + 0] = R0, SH[base + 1] = G0, SH[base + 2] = B0,
        //   SH[base + 3] = R1, ...
        int32 shOutBase = i * SHCoeffCount * 3;
        const int32 SHCoeffsPerChannel = Header.NumSHRestCoeffs / 3;  // 3, 8, or 15
        
        for (int32 c = 0; c < SHCoeffCount; c++)
        {
            // Guard: don't read past the actual f_rest properties in the file
            if (c >= SHCoeffsPerChannel) break;
            
            // Indices into Offset_frest[] (which has exactly NumSHRestCoeffs entries):
            //   R: Offset_frest[c]
            //   G: Offset_frest[SHCoeffsPerChannel + c]
            //   B: Offset_frest[2 * SHCoeffsPerChannel + c]
            float sh_r = ReadFloat(Header.Offset_frest[c]);
            float sh_g = ReadFloat(Header.Offset_frest[SHCoeffsPerChannel + c]);
            float sh_b = ReadFloat(Header.Offset_frest[2 * SHCoeffsPerChannel + c]);
            
            OutData.SphericalHarmonics[shOutBase + c * 3 + 0] = sh_r;
            OutData.SphericalHarmonics[shOutBase + c * 3 + 1] = sh_g;
            OutData.SphericalHarmonics[shOutBase + c * 3 + 2] = sh_b;
        }
        });
    }


    UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: Loaded %d splats with SH degree %d (file %.2f ms, header %.2f ms, decode %.2f ms)"),
        N, OutData.SHDegree, FileReadSeconds * 1000.0, HeaderParseSeconds * 1000.0, VertexDecodeSeconds * 1000.0);
    return true;
}

bool FGaussianSplatPlyLoader::ParseHeader(const TArray64<uint8>& FileData, int64& OutDataOffset, FPlyHeader& OutHeader)
{
    // The PLY header is ASCII text, terminated by "end_header\n"
    // We parse it line by line

    const uint8* Data    = FileData.GetData();
    const int64  DataLen = FileData.Num();

    // Read header as string (scan for "end_header")
    int64 HeaderEnd = -1;
    for (int64 i = 0; i < DataLen - 10; i++)
    {
        if (FMemory::Memcmp(Data + i, "end_header", 10) == 0)
        {
            // Find end of this line
            int64 LineEnd = i + 10;
            while (LineEnd < DataLen && Data[LineEnd] != '\n')
                LineEnd++;
            HeaderEnd = LineEnd + 1;
            break;
        }
    }

    if (HeaderEnd < 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: 'end_header' not found in PLY file"));
        return false;
    }

    OutDataOffset = HeaderEnd;

    // Convert header to string for parsing
    TArray<uint8> HeaderBytes;
    HeaderBytes.Reserve(static_cast<int32>(HeaderEnd) + 1);
    HeaderBytes.Append(Data, static_cast<int32>(HeaderEnd));
    HeaderBytes.Add(0);  // null terminate
    FString HeaderStr = UTF8_TO_TCHAR((const char*)HeaderBytes.GetData());

    TArray<FString> Lines;
    HeaderStr.ParseIntoArrayLines(Lines);

    // Parse header lines
    FString CurrentElement;
    int32   CurrentByteOffset = 0;
    bool    InVertexElement   = false;
    OutHeader.IsBinary        = false;
    
    // Map property name -> byte offset
    TMap<FString, int32> PropOffsets;
    TMap<FString, int32> PropSizes;  // byte size of each property
    
    for (const FString& Line : Lines)
    {
        FString TrimmedLine = Line.TrimStartAndEnd();
        
        if (TrimmedLine.StartsWith(TEXT("format")))
        {
            if (TrimmedLine.Contains(TEXT("binary_little_endian")))
            {
                OutHeader.IsBinary    = true;
                OutHeader.IsBigEndian = false;
            }
            else if (TrimmedLine.Contains(TEXT("binary_big_endian")))
            {
                OutHeader.IsBinary    = true;
                OutHeader.IsBigEndian = true;
            }
            // else: ascii (not supported for large PLY files)
        }
        else if (TrimmedLine.StartsWith(TEXT("element")))
        {
            TArray<FString> Parts;
            TrimmedLine.ParseIntoArray(Parts, TEXT(" "));
            if (Parts.Num() >= 3)
            {
                CurrentElement    = Parts[1];
                InVertexElement   = CurrentElement.Equals(TEXT("vertex"), ESearchCase::IgnoreCase);
                CurrentByteOffset = 0;
                
                if (InVertexElement)
                {
                    OutHeader.NumVertices = FCString::Atoi(*Parts[2]);
                }
            }
        }
        else if (TrimmedLine.StartsWith(TEXT("property")) && InVertexElement)
        {
            TArray<FString> Parts;
            TrimmedLine.ParseIntoArray(Parts, TEXT(" "));
            if (Parts.Num() >= 3)
            {
                FString TypeStr = Parts[1];
                FString Name    = Parts[2];

                int32 PropSize = 0;
                if (TypeStr.Equals(TEXT("float")) || TypeStr.Equals(TEXT("float32")))
                    PropSize = 4;
                else if (TypeStr.Equals(TEXT("double")) || TypeStr.Equals(TEXT("float64")))
                    PropSize = 8;
                else if (TypeStr.Equals(TEXT("int")) || TypeStr.Equals(TEXT("int32")) || TypeStr.Equals(TEXT("uint")) || TypeStr.Equals(TEXT("uint32")))
                    PropSize = 4;
                else if (TypeStr.Equals(TEXT("short")) || TypeStr.Equals(TEXT("int16")) || TypeStr.Equals(TEXT("ushort")) || TypeStr.Equals(TEXT("uint16")))
                    PropSize = 2;
                else if (TypeStr.Equals(TEXT("char")) || TypeStr.Equals(TEXT("int8")) || TypeStr.Equals(TEXT("uchar")) || TypeStr.Equals(TEXT("uint8")))
                    PropSize = 1;
                else
                    PropSize = 4;  // default

                PropOffsets.Add(Name, CurrentByteOffset);
                PropSizes.Add(Name, PropSize);
                CurrentByteOffset += PropSize;
            }
        }
    }

    OutHeader.BytesPerVertex = CurrentByteOffset;

    // Map property names to offsets in the header
    auto GetOffset = [&](const FString& Name) -> int32
    {
        const int32* OffPtr = PropOffsets.Find(Name);
        return OffPtr ? *OffPtr : -1;
    };

    OutHeader.Offset_x       = GetOffset(TEXT("x"));
    OutHeader.Offset_y       = GetOffset(TEXT("y"));
    OutHeader.Offset_z       = GetOffset(TEXT("z"));
    OutHeader.Offset_opacity = GetOffset(TEXT("opacity"));
    
    // DC color
    OutHeader.Offset_fdc[0]  = GetOffset(TEXT("f_dc_0"));
    OutHeader.Offset_fdc[1]  = GetOffset(TEXT("f_dc_1"));
    OutHeader.Offset_fdc[2]  = GetOffset(TEXT("f_dc_2"));
    
    // Scale
    OutHeader.Offset_scale[0] = GetOffset(TEXT("scale_0"));
    OutHeader.Offset_scale[1] = GetOffset(TEXT("scale_1"));
    OutHeader.Offset_scale[2] = GetOffset(TEXT("scale_2"));
    
    // Rotation quaternion (w, x, y, z)
    OutHeader.Offset_rot[0]   = GetOffset(TEXT("rot_0"));
    OutHeader.Offset_rot[1]   = GetOffset(TEXT("rot_1"));
    OutHeader.Offset_rot[2]   = GetOffset(TEXT("rot_2"));
    OutHeader.Offset_rot[3]   = GetOffset(TEXT("rot_3"));
    
    // SH rest coefficients (f_rest_0 ... f_rest_44)
    OutHeader.NumSHRestCoeffs = 0;
    for (int32 r = 0; r < 45; r++)
    {
        FString RestName = FString::Printf(TEXT("f_rest_%d"), r);
        int32 Offset = GetOffset(RestName);
        if (Offset >= 0)
        {
            OutHeader.Offset_frest[r] = Offset;
            OutHeader.NumSHRestCoeffs++;
        }
        else
        {
            break;  // Stop at first missing coefficient
        }
    }
    
    // Validate
    if (!OutHeader.IsBinary)
    {
        UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: ASCII PLY format not supported (too slow). Use binary_little_endian."));
        return false;
    }

    if (OutHeader.NumVertices <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: PLY file has no vertices"));
        return false;
    }

    return true;
}

