#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatAsset.h"

/**
 * Loads 3D Gaussian Splatting data from a binary .ply file
 * 
 * The .ply file format for 3DGS (from the original INRIA codebase) stores:
 *   Element: vertex (one per Gaussian splat)
 *   Properties:
 *     - x, y, z              (float: 3D center position)
 *     - nx, ny, nz           (float: unused normals, always 0)
 *     - f_dc_0, f_dc_1, f_dc_2    (float: SH degree-0 / DC component, RGB)
 *     - f_rest_0 .. f_rest_44     (float: SH degrees 1-3 coefficients, 45 total)
 *     - opacity              (float: logit-space opacity, apply sigmoid to get [0,1])
 *     - scale_0, scale_1, scale_2 (float: log-space scale, apply exp to get actual)
 *     - rot_0, rot_1, rot_2, rot_3 (float: quaternion WXYZ)
 *
 * After loading, the asset importer immediately packs these temporary floats
 * into the compressed runtime payload.
 */
class FGaussianSplatPlyLoader
{
public:
    /**
     * Load a 3DGS .ply file into FGaussianSplatData
     * @param FilePath  Absolute path to the .ply file
     * @param OutData   Output data structure to fill
     * @return true on success
     */
    static bool Load(const FString& FilePath, FGaussianSplatData& OutData);

private:
    /** Parse the PLY header and return the property layout */
    struct FPlyHeader
    {
        int32 NumVertices = 0;
        bool  IsBinary    = true;
        bool  IsBigEndian = false;
        
        // Property byte offsets within a single vertex record
        int32 BytesPerVertex = 0;
        
        // Offset of each property (in bytes from start of vertex)
        int32 Offset_x   = -1;
        int32 Offset_y   = -1;
        int32 Offset_z   = -1;
        int32 Offset_fdc[3]     = {-1, -1, -1};     // f_dc_0, f_dc_1, f_dc_2
        int32 Offset_frest[45];                       // f_rest_0 ... f_rest_44
        int32 Offset_opacity = -1;
        int32 Offset_scale[3]   = {-1, -1, -1};      // scale_0, scale_1, scale_2
        int32 Offset_rot[4]     = {-1, -1, -1, -1};  // rot_0, rot_1, rot_2, rot_3
        
        int32 NumSHRestCoeffs = 0;  // Number of f_rest properties found (0, 9, 24, or 45)
        
        FPlyHeader()
        {
            FMemory::Memset(Offset_frest, 0xFF, sizeof(Offset_frest));
        }
    };
    
    static bool ParseHeader(const TArray64<uint8>& FileData, int64& OutDataOffset, FPlyHeader& OutHeader);

    /** Apply sigmoid: 1 / (1 + exp(-x)) */
    static float Sigmoid(float x) { return 1.0f / (1.0f + FMath::Exp(-x)); }
};
